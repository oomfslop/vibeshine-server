/**
 * @file src/app_launcher.h
 * @brief The app launcher of app-window clients: what the signed-in user can
 *        launch (the Start menu's apps with their icons, the programs on the
 *        PATH) and launching it as that user. The windows it opens are
 *        streamed like any other host window.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace app_launcher {

  struct app_t {
    std::string name;
    std::string id;  ///< parsing name in the shell's Apps folder (AppUserModelID or path)
    std::string icon_key;  ///< "" when it has none
  };

  struct command_t {
    std::string name;  ///< as typed (the file name without .exe)
    std::string path;
  };

  struct list_t {
    std::vector<app_t> apps;
    std::vector<command_t> commands;
    bool ok = false;
  };

  /** What can be launched (asked of the user's shell; kept for a while, or
   *  asked again with `refresh`) */
  list_t list(bool refresh);

  /** An app's icon (PNG) by its key */
  std::optional<std::string> icon_png(const std::string &key);

  /** Launch an app of the list as the signed-in user */
  bool launch_app(const std::string &id, std::string &error);

  /** Run a command line as the signed-in user, from their home folder, with
   *  a console window when the program is a console program */
  bool launch_command(const std::string &command_line, std::string &error);

}  // namespace app_launcher
