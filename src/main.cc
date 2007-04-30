#if defined(USE_BOOST_PYTHON)
#include <pyledger.h>
#else
#include <ledger.h>
#endif
#include <option.h>

#include "acconf.h"

#ifdef HAVE_UNIX_PIPES
#include <sys/types.h>
#include <sys/wait.h>
#include "fdstream.hpp"
#endif

using namespace ledger;

#if 0
class print_addr : public repitem_t::select_callback_t {
  virtual void operator()(repitem_t * item) {
    std::cout << item << std::endl;
  }
};
#endif

static int read_and_report(report_t * report, int argc, char * argv[],
			   char * envp[])
{
  session_t& session(*report->session);

  // Handle the command-line arguments

  std::list<string> args;
  process_arguments(argc - 1, argv + 1, false, report, args);

  if (args.empty()) {
#if 0
    help(std::cerr);
#endif
    return 1;
  }
  strings_list::iterator arg = args.begin();

  if (session.cache_file == "<none>")
    session.use_cache = false;
  else
    session.use_cache = session.data_file.empty() && session.price_db.empty();

  DEBUG("ledger.session.cache", "1. use_cache = " << session.use_cache);

  // Process the environment settings

  TRACE_START(environment, 1, "Processed environment variables");
  process_environment(const_cast<const char **>(envp), "LEDGER_", report);
  TRACE_FINISH(environment, 1);

  const char * p = std::getenv("HOME");
  string home = p ? p : "";

  if (session.init_file.empty())
    session.init_file  = home + "/.ledgerrc";
  if (session.price_db.empty())
    session.price_db   = home + "/.pricedb";

  if (session.cache_file.empty())
    session.cache_file = home + "/.ledger-cache";

  if (session.data_file == session.cache_file)
    session.use_cache = false;

  DEBUG("ledger.session.cache", "2. use_cache = " << session.use_cache);

  INFO("Initialization file is " << session.init_file);
  INFO("Price database is " << session.price_db);
  INFO("Binary cache is " << session.cache_file);
  INFO("Journal file is " << session.data_file);

  if (! session.use_cache)
    INFO("Binary cache mechanism will not be used");

  // Read the command word and create a command object based on it

  string verb = *arg++;

  std::auto_ptr<xml::xpath_t::functor_t> command;

  if (verb == "register" || verb == "reg" || verb == "r") {
#if 1
    command.reset(new register_command);
#else
    command = new format_command
      ("register", either_or(report->format_string,
			     report->session->register_format));
#endif
  }
#if 0
  else if (verb == "balance" || verb == "bal" || verb == "b") {
    if (! report->raw_mode) {
      report->transforms.push_back(new accounts_transform);
      report->transforms.push_back(new clean_transform);
      report->transforms.push_back(new compact_transform);
    }
    command = new format_command
      ("balance", either_or(report->format_string,
			     report->session->balance_format));
  }
  else if (verb == "print" || verb == "p") {
    if (! report->raw_mode)
      report->transforms.push_back(new optimize_transform);
    command = new format_command
      ("print", either_or(report->format_string,
			  report->session->print_format));
  }
  else if (verb == "equity") {
    if (! report->raw_mode)
      report->transforms.push_back(new accounts_transform);
    command = new format_command
      ("equity", either_or(report->format_string,
			   report->session->equity_format));
  }
  else if (verb == "entry")
    command = new entry_command;
  else if (verb == "dump")
    command = new dump_command;
  else if (verb == "output")
    command = new output_command;
  else if (verb == "prices")
    command = new prices_command;
  else if (verb == "pricesdb")
    command = new pricesdb_command;
  else if (verb == "csv")
    command = new csv_command;
  else if (verb == "emacs" || verb == "lisp")
    command = new emacs_command;
#endif
  else if (verb == "xml")
    command.reset(new xml_command);
  else if (verb == "expr")
    ;
  else if (verb == "xpath")
    ;
  else if (verb == "parse") {
    xml::xpath_t expr(*arg);

    IF_INFO() {
      std::cout << "Value expression tree:" << std::endl;
      expr.dump(std::cout);
      std::cout << std::endl;
      std::cout << "Value expression parsed was:" << std::endl;
      expr.write(std::cout);
      std::cout << std::endl << std::endl;
      std::cout << "Result of calculation: ";
    }

    std::cout << expr.calc((xml::document_t *)NULL, report).
      strip_annotations() << std::endl;

    return 0;
  }
  else {
    char buf[128];
    std::strcpy(buf, "command_");
    std::strcat(buf, verb.c_str());

    // jww (2007-04-19): This is an error, since command is an
    // auto_ptr!
    if (xml::xpath_t::op_t * def = report->lookup(buf))
      command.reset(def->functor_obj());

    if (! command.get())
      throw_(exception, string("Unrecognized command '") + verb + "'");
  }

  // Parse the initialization file, which can only be textual; then
  // parse the journal data.

  session.read_init();

  INFO_START(journal, "Read journal file");
  journal_t * journal = session.read_data(report->account);
  INFO_FINISH(journal);

  TRACE_FINISH(entry_text, 1);
  TRACE_FINISH(entry_date, 1);
  TRACE_FINISH(entry_details, 1);
  TRACE_FINISH(entry_xacts, 1);
  TRACE_FINISH(entries, 1);
  TRACE_FINISH(parsing_total, 1);

  // Configure the output stream

#ifdef HAVE_UNIX_PIPES
  int status, pfd[2];		// Pipe file descriptors
#endif
  std::ostream * out = &std::cout;

  if (! report->output_file.empty()) {
    out = new std::ofstream(report->output_file.c_str());
  }
#ifdef HAVE_UNIX_PIPES
  else if (! report->pager.empty()) {
    status = pipe(pfd);
    if (status == -1)
      throw_(exception, "Failed to create pipe");

    status = fork();
    if (status < 0) {
      throw_(exception, "Failed to fork child process");
    }
    else if (status == 0) {	// child
      const char *arg0;

      // Duplicate pipe's reading end into stdin
      status = dup2(pfd[0], STDIN_FILENO);
      if (status == -1)
	perror("dup2");

      // Close unuseful file descriptors: the pipe's writing and
      // reading ends (the latter is not needed anymore, after the
      // duplication).
      close(pfd[1]);
      close(pfd[0]);

      // Find command name: its the substring starting right of the
      // rightmost '/' character in the pager pathname.  See manpage
      // for strrchr.
      arg0 = std::strrchr(report->pager.c_str(), '/');
      if (arg0)
	arg0++;
      else
	arg0 = report->pager.c_str(); // No slashes in pager.

      execlp(report->pager.c_str(), arg0, (char *)0);
      perror("execl");
      exit(1);
    }
    else {			// parent
      close(pfd[0]);
      out = new boost::fdostream(pfd[1]);
    }
  }
#endif

  // Are we handling the expr commands?  Do so now.

  if (verb == "expr") {
    xml::xpath_t expr(*arg);

    IF_INFO() {
      *out << "Value expression tree:" << std::endl;
      expr.dump(*out);
      *out << std::endl;
      *out << "Value expression parsed was:" << std::endl;
      expr.write(*out);
      *out << std::endl << std::endl;
      *out << "Result of calculation: ";
    }

    *out << expr.calc((xml::document_t *)NULL, report).
      strip_annotations() << std::endl;

    return 0;
  }
  else if (verb == "xpath") {
    std::cout << "XPath parsed:" << std::endl;
    xml::xpath_t xpath(*arg);
    xpath.write(*out);
    *out << std::endl;

#if 0
    std::auto_ptr<repitem_t> items(repitem_t::wrap(&session, report, true));
    print_addr cb;
    items->select(path.get(), cb);
#endif
    return 0;
  }

  // Create the an argument scope containing the report command's
  // arguments, and then invoke the command.

  std::auto_ptr<xml::xpath_t::scope_t> locals
    (new xml::xpath_t::scope_t(report, xml::xpath_t::scope_t::ARGUMENT));

  locals->args = new value_t::sequence_t;
  locals->args.push_back(out);
  locals->args.push_back(journal->document);

  if (command->wants_args) {
    for (strings_list::iterator i = args.begin();
	 i != args.end();
	 i++)
      locals->args.push_back(*i);
  } else {
    string regexps[4];

    // Treat the remaining command-line arguments as regular
    // expressions, used for refining report results.

    int base = 0;
    for (strings_list::iterator i = arg; i != args.end(); i++)
      if ((*i)[0] == '-') {
	if ((*i)[1] == '-') {
	  if (base == 0)
	    base += 2;
	  continue;
	}
	if (! regexps[base + 1].empty())
	  regexps[base + 1] += "|";
	regexps[base + 1] += (*i).substr(1);
      } else {
	if (! regexps[base].empty())
	  regexps[base] += "|";
	regexps[base] += *i;
      }

#if 0
    // jww (2006-09-21): Escape the \ in these strings!

    if (! regexps[3].empty())
      report->transforms.push_front
	(new remove_transform
	 (string("//entry[payee =~ /(") + regexps[3] + ")/]"));

    if (! regexps[2].empty())
      report->transforms.push_front
	(new select_transform
	 (string("//entry[payee =~ /(") + regexps[2] + ")/]"));

    if (! regexps[1].empty())
      report->transforms.push_front
	(new remove_transform
	 (string("//xact[account =~ /(") + regexps[1] + ")/]"));

    if (! regexps[0].empty())
      report->transforms.push_front
	(new select_transform
	 (string("//xact[account =~ /(") + regexps[0] + ")/]"));
#endif
  }

  INFO_START(transforms, "Applied transforms");
  report->apply_transforms(journal->document);
  INFO_FINISH(transforms);

  INFO_START(command, "Did user command '" << verb << "'");
  value_t temp;
  (*command)(temp, locals.get());
  INFO_FINISH(command);

  // Write out the binary cache, if need be

  if (session.use_cache && session.cache_dirty &&
      ! session.cache_file.empty()) {
    TRACE_START(binary_cache, 1, "Wrote binary journal file");

    std::ofstream stream(session.cache_file.c_str());
#if 0
    write_binary_journal(stream, journal);
#endif

    TRACE_FINISH(binary_cache, 1);
  }

#if defined(FREE_MEMORY)
  // Cleanup memory -- if this is a beta or development build.

  if (! report->output_file.empty())
    delete out;
#endif

  // If the user specified a pager, wait for it to exit now

#ifdef HAVE_UNIX_PIPES
  if (report->output_file.empty() && ! report->pager.empty()) {
    delete out;
    close(pfd[1]);

    // Wait for child to finish
    wait(&status);
    if (status & 0xffff != 0)
      throw_(exception, "Something went wrong in the pager");
  }
#endif

  return 0;
}

int main(int argc, char * argv[], char * envp[])
{
  int status = 1;

  for (int i = 1; i < argc; i++)
    if (argv[i][0] == '-') {
#if defined(VERIFY_ON)
      if (std::strcmp(argv[i], "--verify") == 0)
	ledger::verify_enabled = true;
#endif
#if defined(DEBUG_ON)
      if (i + 1 < argc && std::strcmp(argv[i], "--debug") == 0) {
	ledger::_log_level    = LOG_DEBUG;
	ledger::_log_category = argv[i + 1];
	i++;
      }
#endif
#if defined(TRACING_ON)
      if (i + 1 < argc && std::strcmp(argv[i], "--trace") == 0) {
	ledger::_log_level   = LOG_TRACE;
	ledger::_trace_level = std::atoi(argv[i + 1]);
	i++;
      }
#endif
    }

  try {
    std::ios::sync_with_stdio(false);

    ledger::initialize();

#if ! defined(FULL_DEBUG)
    ledger::do_cleanup = false;
#endif
    INFO("Ledger starting");

    std::auto_ptr<ledger::session_t> session(new ledger::session_t);

#if 0
    session->register_parser(new binary_parser_t);
#endif
#if defined(HAVE_EXPAT) || defined(HAVE_XMLPARSE)
    session->register_parser(new xml::xml_parser_t);
    session->register_parser(new gnucash_parser_t);
#endif
#ifdef HAVE_LIBOFX
    session->register_parser(new ofx_parser_t);
#endif
    session->register_parser(new qif_parser_t);
    session->register_parser(new textual_parser_t);

    std::auto_ptr<ledger::report_t> report(new ledger::report_t(session.get()));

    status = read_and_report(report.get(), argc, argv, envp);

    if (! ledger::do_cleanup) {
      report.release();
      session.release();
    }
  }
#if 0
  catch (error * err) {
    std::cout.flush();
    // Push a null here since there's no file context
    if (err->context.empty() ||
	! dynamic_cast<xact_context *>(err->context.front()))
      err->context.push_front(new error_context(""));
    err->reveal_context(std::cerr, "Error");
    std::cerr << err->what() << std::endl;
    delete err;
  }
  catch (fatal * err) {
    std::cout.flush();
    // Push a null here since there's no file context
    if (err->context.empty() ||
	! dynamic_cast<xact_context *>(err->context.front()))
      err->context.push_front(new error_context(""));
    err->reveal_context(std::cerr, "Fatal");
    std::cerr << err->what() << std::endl;
    delete err;
  }
#endif
  catch (const std::exception& err) {
    std::cout.flush();
    std::cerr << "Error: " << err.what() << std::endl;
  }
  catch (int _status) {
    status = _status;
  }

  if (ledger::do_cleanup)
    ledger::shutdown();

  return status;
}

// main.cc ends here.