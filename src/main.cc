#include <unistd.h>

#include <QApplication>

#include "app_init.h"
#include "cli.h"
#include "dialog.h"
#include "wizard/wizard.h"
#include "sigsource.h"

int main(int argc, char *argv[]) {
  Config config;
  QCommandLineParser parser;
  if (!initApp(&argc, argv, &config, &parser).ok()) return 1;

  if (argc == 1 || parser.isSet("gui") || parser.isSet("wizard")) {
    // Run in GUI mode.
    QApplication app(argc, argv);
    parser.process(app);
    config.fromCommandLine(parser);
    app.setApplicationDisplayName("Mongoose IoT flashing tool");
    std::unique_ptr<QMainWindow> w;
    if (parser.isSet("wizard")) {
      w.reset(new WizardDialog(&config));
    } else {
      MainDialog *md = new MainDialog(&config);
      SigSource *ss = initSignalSource(md);
      QObject::connect(ss, &SigSource::flash, md, &MainDialog::flashClicked);
      QObject::connect(ss, &SigSource::connectDisconnect, md,
                       &MainDialog::connectDisconnectTerminal);
      w.reset(md);
    }
    w->show();
    _exit(app.exec());
  }

  // Run in CLI mode.
  QCoreApplication app(argc, argv);
  parser.process(app);
  config.fromCommandLine(parser);
  CLI cli(&config, &parser);
  _exit(app.exec());
  return 1;
}
