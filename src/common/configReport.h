#ifndef COMMON_CONFIG_REPORT_H_
#define COMMON_CONFIG_REPORT_H_

#include <string>

// The effective configuration, as "CONFIG ..." lines (docs/mt-rewrite/SWITCHES.md):
//   startup   build mode and profile, version, every KYTY_* switch set in the environment (its
//             kind, default and effective value), unknown switch names, meaningless combinations;
//   features  each optional feature's state once it is decided (active, or the fallback and why);
//   summary   both again as one block ("CONFIG summary begin" ... "end") after the game's 120th
//             flip, in the log file too. The bench scripts save that block with each run.
namespace Common::ConfigReport {

// The startup block, on stdout (the log is not open yet). version: one line naming the binary.
void PrintStartup(const std::string& version);

// A feature's state: "<name>: <state>", e.g. ("write tracking", "async write protection
// (userfaultfd WP_ASYNC + PAGEMAP_SCAN)"). Printed at once; repeated in the summary.
void Feature(const std::string& name, const std::string& state);

// Called at every flip; prints the summary once, at the 120th.
void OnFlip();

} // namespace Common::ConfigReport

#endif // COMMON_CONFIG_REPORT_H_
