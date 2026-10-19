#pragma once

#include <filesystem>

// Finder launch (Driveclub.app, no game dir on the command line): logs go to
// ~/Library/Logs/DriveclubNative, the game folder is picked in a dialog and
// remembered, and problems are reported in alerts instead of on stderr.

namespace Launcher {

// True when there is no game dir on the command line.
bool IsInteractive(int argc, char** argv);

// Interactive only: redirects the log to a file and, if the last run did not
// end cleanly, copies its log to the Desktop and offers to show it. Call
// before anything logs.
void BeginSession();

// Marks the run as ended cleanly (no crash alert on the next launch).
void EndSession();

// The remembered game folder, or one picked in a dialog (hold Option at launch
// to pick again). Empty if the user cancels.
std::filesystem::path ChooseGameFolder();

// Alert with a "Show Log" button when the log is a file; the log is copied to
// ~/Desktop/Driveclub-log.txt first.
void ShowError(const char* title, const char* text);

// Unsupported game version: offers to pick another folder (relaunches the
// app) or quit. Does not return.
[[noreturn]] void RejectGameFolder(const std::filesystem::path& dir);

} // namespace Launcher
