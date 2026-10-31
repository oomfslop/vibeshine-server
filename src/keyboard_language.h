/**
 * @file src/keyboard_language.h
 * @brief Types in the language of the client's keyboard: the host's input
 *        language follows the one the client picked, with one of the
 *        signed-in user's keyboard layouts for it.
 */
#pragma once

#include <string>

namespace keyboard_language {

  /** The client picked this language ("en", "ru", "zh-Hans", ...): the
   *  foreground window takes the user's layout for it, and so does every
   *  window that comes to the foreground while it holds. False when the user
   *  has no layout for it. */
  bool set(const std::string &language, std::string &error);

  /** Streams ended: the host's own choice holds again */
  void clear();

}  // namespace keyboard_language
