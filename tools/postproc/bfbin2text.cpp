/*******************************************************
 * Convert Byfl binary data to various textual formats *
 * By Scott Pakin <pakin@lanl.gov>                     *
 *******************************************************/

#include <iostream>
#include <fstream>
#include <unordered_set>
#include <functional>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include "bfbin.h"

using namespace std;

// Define the name of the current executable.
string progname;

// Abort the program.  This is expected to be used at the end of a
// stream write.
static ostream& die (ostream& os)
{
  os.flush();
  exit(1);
  return os;
}

// Define a type for our local parsing state.
class LocalState {
private:
  string expand_escapes (string& in_str);

public:
  string infilename;      // Name of the input file
  string outfilename;     // Name of the output file
  ostream* outfile;       // Output file stream
  size_t tablenum;        // Current table number
  string colsep;          // Column separator or empty for "smart" spaces
  size_t colnum;          // Current column number (header or data row)
  unordered_set<string> included_tables;   // Set of tables to include
  unordered_set<string> excluded_tables;   // Set of tables to exclude
  bool suppress_table;    // true=skip the current table; false=show it
  bool only_names;        // true=show only table names; false=show everything

  LocalState (int argc, char* argv[]);
  ~LocalState();
};

// Parse the command line into a LocalState.
LocalState::LocalState (int argc, char* argv[])
{
  // Initialize the current state.
  infilename = "";
  outfilename = "";
  outfile = &cout;
  tablenum = 0;
  colsep = ",";
  colnum = 0;
  suppress_table = false;
  only_names = false;

  // Walk the command line and process each option we encounter.
  static struct option cmd_line_options[] = {
    { "output",  required_argument, NULL, 'o' },
    { "colsep",  required_argument, NULL, 'c' },
    { "include", required_argument, NULL, 'i' },
    { "exclude", required_argument, NULL, 'e' },
    { "list",    no_argument,       NULL, 'l' },
    { NULL,      0,                 NULL, 0 }
  };
  int opt_index = 0;
  while (true) {
    int c = getopt_long(argc, argv, "o:c:i:e:l", cmd_line_options, &opt_index);
    if (c == -1)
      break;
    switch (c) {
      case 'o':
        outfilename = string(optarg);
        break;

      case 'c':
        colsep = string(optarg);
        colsep = expand_escapes(colsep);
        break;

      case 'i':
        included_tables.emplace(optarg);
        break;

      case 'e':
        excluded_tables.emplace(optarg);
        break;

      case 'l':
        only_names = true;
        break;

      case 0:
        cerr << progname << ": Internal error in " << __FILE__
             << ", line " << __LINE__ << '\n' << die;
        break;

      default:
        exit(1);
        break;
    }
  }

  // Parse the remaining non-option, if any.
  switch (argc - optind) {
    case 1:
      // Exactly one argument: Store it as the input file name.
      infilename = string(argv[optind]);
      break;

    case 0:
      // No arguments: Complain.
      cerr << progname << ": The name of a Byfl binary file must be specified\n" << die;
      break;

    default:
      // More than one argument: Complain.
      cerr << progname << ": Only a single input file is allowed to be specified\n" << die;
      break;
  }

  // Ensure that tables are either included or excluded, not both.
  if (excluded_tables.size() > 0 && included_tables.size() > 0)
    cerr << progname << ": Only one of --include (-i) and --exclude (-e)"
         << " may be specified\n" << die;

  // Open the output file if specified.
  if (outfilename != "") {
    ofstream* named_outfile = new ofstream(outfilename, ofstream::trunc|ofstream::binary);
    if (!named_outfile->is_open())
      cerr << progname << ": Failed to open " << outfilename
           << " for writing\n" << die;
    outfile = named_outfile;
  }
}

// Flush the output stream and close it if it's a file.
LocalState::~LocalState()
{
  if (outfilename != "")
    delete outfile;
  else
    outfile->flush();
}

// Replace "\t" with tab and "\n" with newline.
string LocalState::expand_escapes (string& in_str)
{
  string out_str;              // String to return
  bool escape_next = false;    // true=next character is part of an escape sequence; false=ordinary character
  for (auto iter = in_str.cbegin(); iter != in_str.cend(); iter++) {
    char c = *iter;
    if (escape_next) {
      // The previous character was a backslash.
      switch (c) {
        case '\\':
        case '\'':
        case '"':
          out_str += c;
          break;

        case 't':
          out_str += '\t';
          break;

        case 'n':
          out_str += '\n';
          break;

        case 'r':
          out_str += '\r';
          break;

        default:
          cerr << progname << ": Unrecognized escape sequence \"\\" << c
               << "\" in \"" << in_str << "\"\n" << die;
          break;
      }
      escape_next = false;
    }
    else
      // The previous character was not a backslash.
      switch (c) {
        case '\\':
          escape_next = true;
          break;

        default:
          out_str += c;
          break;
      }
  }
  return out_str;
}

// Quote a string for CSV output.  For now, we surround all strings
// with double quotes, even though they're technicaly required only
// for strings containing commas.  We do, however, escape internal
// double quotes by doubling them.  (Both LibreOffice and Microsoft
// Excel seem to honor that convention.)
static string quote_for_csv (const string& in_str)
{
  string out_str;
  if (in_str.length() > 0 && in_str[0] == '-')
    out_str += '=';   // Required by Excel; accepted by LibreOffice
  out_str += '"';
  for (auto iter = in_str.cbegin(); iter != in_str.cend(); iter++) {
    if (*iter == '"')
      out_str += '"';
    out_str += *iter;
  }
  out_str += '"';
  return out_str;
}

// Do the same as the above but accept an rvalue.
static string quote_for_csv (const string&& in_str)
{
  return quote_for_csv(ref(in_str));
}

// Report a parse error and abort.
#pragma GCC diagnostic ignored "-Wunused-parameter"
static void error_callback (void* state, const char* message)
{
  cerr << progname << ": " << message << endl << die;
}

// Begin outputting a table (either type).
static void begin_any_table (void* state, const char* tablename)
{
  LocalState* lstate = (LocalState*) state;
  string name(tablename);

  // Determine if we should show or suppress the current table.
  lstate->suppress_table =
    (lstate->included_tables.size() > 0 && lstate->included_tables.find(name) == lstate->included_tables.cend())
    || lstate->excluded_tables.find(name) != lstate->excluded_tables.cend();
  if (lstate->suppress_table)
    return;

  // Output the name of the current table.
  if (lstate->only_names)
    lstate->suppress_table = true;
  else
    if (lstate->tablenum++ > 0)
      *lstate->outfile << '\n';
  *lstate->outfile << quote_for_csv(name) << '\n';
}

// Begin outputting a column header or a row of data.
static void begin_row (void* state)
{
  LocalState* lstate = (LocalState*) state;
  if (lstate->suppress_table)
    return;
  lstate->colnum = 0;
}

// Output the name of any column type.
static void any_column_header (void* state, const char* colname)
{
  LocalState* lstate = (LocalState*) state;
  if (lstate->suppress_table)
    return;
  if (lstate->colnum > 0)
    *lstate->outfile << lstate->colsep;
  *lstate->outfile << quote_for_csv(colname);
  lstate->colnum++;
}

// Finish outputting a column header or row of data.
static void end_row (void* state)
{
  LocalState* lstate = (LocalState*) state;
  if (lstate->suppress_table)
    return;
  *lstate->outfile << '\n';
}

// Write a 64-bit unsigned integer value.
static void write_uint64_value (void* state, uint64_t value)
{
  LocalState* lstate = (LocalState*) state;
  if (lstate->suppress_table)
    return;
  if (lstate->colnum > 0)
    *lstate->outfile << lstate->colsep;
  *lstate->outfile << value;
  lstate->colnum++;
}

// Write a string value.
static void write_string_value (void* state, const char* value)
{
  LocalState* lstate = (LocalState*) state;
  if (lstate->suppress_table)
    return;
  if (lstate->colnum > 0)
    *lstate->outfile << lstate->colsep;
  *lstate->outfile << quote_for_csv(value);
  lstate->colnum++;
}

// Write a boolean value.
static void write_bool_value (void* state, uint8_t value)
{
  LocalState* lstate = (LocalState*) state;
  if (lstate->suppress_table)
    return;
  if (lstate->colnum > 0)
    *lstate->outfile << lstate->colsep;
  *lstate->outfile << (value == 0 ? "FALSE" : "TRUE");
  lstate->colnum++;
}

int main (int argc, char *argv[])
{
  string byfl_filename;      // Name of input file
  string text_filename;      // Name of output file

  // Store the base filename of the current executable in progname.
  progname = argv[0];
  size_t slash_ofs = progname.rfind('/');
  if (slash_ofs != string::npos)
    progname.erase(0, slash_ofs + 1);

  // Parse the command line.
  LocalState state(argc, argv);

  // Register callbacks.
  bfbin_callback_t callbacks;
  memset(&callbacks, 0, sizeof(bfbin_callback_t));
  callbacks.error_cb = error_callback;
  callbacks.table_begin_basic_cb = begin_any_table;
  callbacks.table_begin_keyval_cb = begin_any_table;
  callbacks.column_begin_cb = begin_row;
  callbacks.column_uint64_cb = any_column_header;
  callbacks.column_string_cb = any_column_header;
  callbacks.column_bool_cb = any_column_header;
  callbacks.column_end_cb = end_row;
  callbacks.row_begin_cb = begin_row;
  callbacks.data_uint64_cb = write_uint64_value;
  callbacks.data_string_cb = write_string_value;
  callbacks.data_bool_cb = write_bool_value;
  callbacks.row_end_cb = end_row;

  // Process the input file.
  bf_process_byfl_file(state.infilename.c_str(),
                       &callbacks, sizeof(bfbin_callback_t),
                       &state);

  return 0;
}
