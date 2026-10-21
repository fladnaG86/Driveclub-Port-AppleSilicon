#pragma once

#include <filesystem>

// Finder launch (Driveclub.app, no game dir on the command line): each session
// logs to its own file in "Driveclub Logs" on the Desktop, the game folder is
// picked in a dialog and remembered, and problems are reported in alerts
// instead of on stderr.

namespace Launcher {

// True when there is no game dir on the command line.
bool IsInteractive(int argc, char** argv);

// Interactive only: redirects the log to this session's file and, if the
// last run did not end cleanly, tags its log "-crash" and offers to show it.
// Call before anything logs.
void BeginSession();

// Marks the run as ended cleanly (no crash alert on the next launch).
void EndSession();

// Folder holding the session log, or empty when running from the command line.
std::filesystem::path LogDir();

// The remembered game folder, or one picked in a dialog (hold Option at launch
// to pick again). Empty if the user cancels.
std::filesystem::path ChooseGameFolder();

// Alert with a "Show Log" button when the log is a file, which gets tagged
// "-error".
void ShowError(const char* title, const char* text);

// Unsupported game version: offers to pick another folder (relaunches the
// app) or quit. Does not return.
[[noreturn]] void RejectGameFolder(const std::filesystem::path& dir);

} // namespace Launcher
