<?php
/**
 * @file
 * Define a MigrateSource for importing from Google spreadsheets.
 */

/**
 * Implements MigrateSource, to import from Google spreadsheets.
 */
class MigrateSourceGoogleSpreadsheet extends MigrateSource {
  /**
   * Google Worksheets API cell-based feed URI.
   *
   * @var string
   */
  protected $feedUri = 'https://spreadsheets.google.com/feeds/cells/__key__/__sheet__/public/basic';

  /**
   * Spreadsheet unique key.
   *
   * @var string
   */
  protected $key = '';

  /**
   * Worksheet ID.
   *
   * @var integer
   */
  protected $worksheet = 1;

  /**
   * Worksheet title
   *
   * @var string
   */
  protected $worksheetTitle = '';

  /**
   * A unique id for this migration, used in __toString().
   *
   * @var string
   */
  protected $sourceId = '';

  /**
   * The row number of the header row in the sheet.
   * Since we're mapping to column name (A, B etc), a 0
   * value is allowed and indicates no header row.
   *
   * @var integer
   */
  protected $headerRow = 1;

  /**
   * A map of header field values to column names if available.
   *
   * @var array
   */
  protected $header = array();

  /**
   * The actual data - an array of rows which holds an array of cols.
   *
   * @var array
   */
  protected $rows = array();

  /**
   * List of available source fields.
   *
   * @var array
   */
  protected $fields = array();

  /**
   * The current row number in the sheet.
   *
   * @var integer
   */
  protected $rowNumber;

  /**
   * Filter callbacks
   *
   * @var array
   *
   * Motivation: sometimes spreadsheet have rows that are not ready yet to be
   * imported (e.g. no primary key value, or incomplete data). Using filters
   * it's possible to completely ignore those rows.
   */
  protected $filters = array();

  /**
   * Simple initialization.
   *
   * @param array $options
   *   Options applied to this source.
   *   - 'key': The unique key for the spreadsheet. *Required.
   *   - 'worksheet_id': The optional id of the sheet to be processed.
   *   - 'fields': An optional array of fields. Keys are field names,
   *     values are descriptions. Use to override the default descriptions,
   *     or to add additional source fields which the migration will add
   *     via other means (e.g., prepareRow()).
   *   - 'filters': array of callbacks used to filter results
   *
   * @throws Exception
   */
  public function __construct(array $options) {
    parent::__construct($options);

    if (empty($options['key'])) {
      throw new Exception(t('The spreadsheet key is required.'));
    }
    $this->key = $options['key'];

    if (!empty($options['worksheet'])) {
      $this->worksheet = (int)$options['worksheet'];
    }

    if (!empty($options['fields'])) {
      $this->fields = $options['fields'];
    }

    // Set / disable the header row if needed (defaults to row 1).
    if (isset($options['header_row'])) {
      $this->headerRow = (int)$options['header_row'];
    }

    if (isset($options['filters'])) {
      $this->filters = $options['filters'];
    }

    // Load the worksheet.
    $this->load();
  }

  /**
   * Loads the worksheet into memory.
   *
   * Row numbers start from 0 and do not match the worksheet row
   * numbers. Cols on the other hand use 'A', 'B', 'C' to match
   * the worksheet.
   *
   * @return bool
   *   Returns true if the worksheet was successfully loaded, otherwise false.
   */
  public function load() {
    $url = str_replace(array('__key__', '__sheet__'), array($this->key, $this->worksheet), $this->feedUri);
    $response = drupal_http_request($url);
    if ($response->code != 200) {
      Migration::displayMessage(t(
        'Loading of worksheet %worksheet in the %sheet_id spreadsheet failed with %status - %data',
        array('%worksheet' => $this->worksheet, '%sheet_id' => $this->key, '%status' => $response->status_message, '%data' => $response->data)
      ));
      return FALSE;
    }
    libxml_use_internal_errors(TRUE);
    $xml = simplexml_load_string($response->data, null, LIBXML_NOCDATA);
    if ($xml === FALSE) {
      Migration::displayMessage(t(
        'Loading of worksheet %worksheet in the %sheet_id spreadsheet failed. SimpleXML could not parse the response.',
        array('%worksheet' => $this->worksheet, '%sheet_id' => $this->key)
      ));
      // This spits out LOTS of (not necessarily useful) errors.
      /*foreach (libxml_get_errors() as $error) {
        Migration::displayMessage(self::parseLibXMLError($error));
      }*/
      return FALSE;
    }

    // Set the source id.
    $this->sourceId = $xml->title . ' | ' . $xml->updated;
    $this->worksheetTitle = (string)$xml->title;

    // Extract the rows from the XML.
    $this->rows = array();
    if (!empty($xml->entry)) {
      foreach($xml->entry as $entry) {
        preg_match('/([a-z]+)(\d+)/iu', (string)$entry->title, $address);
        $col = $address[1];
        $row = $address[2];

        // Keep the row numbers aligned for now so we can extract
        // the header later.
        $this->rows[$row][$col] = (string)$entry->content;
      }

      // Extract the header row from the normal rows.
      if ($this->headerRow && !empty($this->rows[$this->headerRow])) {
        $this->header = (array)$this->rows[$this->headerRow];
        unset($this->rows[$this->headerRow]);
      }
    }
    $this->filterRows();

    // Reset the row numbers.
    $this->rows = array_values($this->rows);
    return TRUE;
  }

  /**
   * Returns a list of fields available to be mapped.
   *
   * @return array
   *   Keys: machine names of the fields (to be passed to addFieldMapping).
   *   Values: Human-friendly descriptions of the fields.
   */
  public function fields() {
    $fields = array();

    // Add the header row as is.
    if ($this->header) {
      $fields = $this->header;
    }

    // Any caller-specified fields with the same names as extracted
    // fields will override them; any others will be added.
    if ($this->fields) {
      $fields = $this->fields + $fields;
    }

    return $fields;
  }

  /**
   * Return a string representing the source, for display in the UI.
   *
   * Used as a cacheKey for Source::count() and displayed over the source
   * available fields as query.
   */
  public function __toString() {
    return $this->sourceId;
  }

  /**
   * Return the number of available source records.
   */
  public function computeCount() {
    return count($this->rows);
  }

  /**
   * Implementation of MigrateSource::performRewind().
   *
   * @return void
   */
  public function performRewind() {
    $this->rowNumber = 0;
  }

  /**
   * Implementation of MigrateSource::getNextRow().
   * Return the next line of the source sheet as an object.
   *
   * @return null|object
   */
  public function getNextRow() {
    ++$this->rowNumber;
    return !empty($this->rows[$this->rowNumber - 1]) ? (object) $this->rows[$this->rowNumber - 1] : NULL;
  }

  /**
   * Custom functionality: get the title of the worksheet
   *
   * @return string
   */
  public function getWorksheetTitle() {
    return $this->worksheetTitle;
  }

  /**
   * Parses a LibXMLError to a error message string.
   *
   * @param LibXMLError $error
   *   Error thrown by the XML
   *
   * @return string
   *   Error message
   */
  public static function parseLibXMLError(LibXMLError $error) {
    dpm($error, '$error');
    return '';
    $error_code_name = 'Unknown Error';
    switch ($error->level) {
      case LIBXML_ERR_WARNING:
        $error_code_name = t('Warning');
        break;

      case LIBXML_ERR_ERROR:
        $error_code_name = t('Error');
        break;

      case LIBXML_ERR_FATAL:
        $error_code_name = t('Fatal Error');
        break;
    }

    return t(
      "!libxmlerrorcodename !libxmlerrorcode: !libxmlerrormessage\n" .
      "Line: !libxmlerrorline\n" .
      "Column: !libxmlerrorcolumn\n" .
      "File: !libxmlerrorfile",
      array(
        '!libxmlerrorcodename' => $error_code_name,
        '!libxmlerrorcode' => $error->code,
        '!libxmlerrormessage' => trim($error->message),
        '!libxmlerrorline' => $error->line,
        '!libxmlerrorcolumn' => $error->column,
        '!libxmlerrorfile' => (($error->file)) ? $error->file : NULL,
      )
    );
  }

  /**
   * Register a filter callback
   */
  public function filterRows() {
    foreach($this->filters as $filter) {
      if (is_callable($filter)) {
        $this->rows = array_filter($this->rows, $filter);
      }
    }
  }
}
