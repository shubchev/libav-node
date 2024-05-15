#include "common.h"

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE hPrevInstance,
                    PWSTR pCmdLine,
                    int nCmdShow) {
#else
int main(int argc, char **argv) {
#endif

  std::string instanceId;

  CLI::App app("libAV Node Service");
  app.add_option("-i", instanceId, "Service instance. Required unless a test is ran");
  app.add_flag("--log", dumpLog, "Save logs to a file");

#ifdef _WIN32
  try {
    app.parse(pCmdLine);
  } catch (const CLI::ParseError &e) {
    printf(app.help().c_str());
    return 1;
  }
#else
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    printf(app.help().c_str());
    return 1;
  }
#endif

  if (dumpLog) {
    std::string fname = "libav-node-" + instanceId + ".log";
    std::remove(fname.c_str());
    static plog::RollingFileAppender<plog::TxtFormatter> fileAppender(fname.c_str());
    plog::init(plog::debug, &fileAppender);
  }

  if (!startService(instanceId)) {
    LOG_ERROR << "Failed to start the service";
    return 2;
  }
  waitServiceToExit();

  return 0;
}