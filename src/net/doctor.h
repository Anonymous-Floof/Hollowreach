// `--net-doctor`: everything you need to know about why two machines cannot see
// each other, printed in one go, on the machine having the problem.
//
// This exists because diagnosing it once took a screenshot of the Windows
// firewall, an elevated shell and a lot of back and forth, and every fact needed
// was one the game already knew. Discovery has a small number of ways to fail and
// they are all invisible from inside the game: a beacon sent out the wrong
// adapter, a port already taken, two machines on different protocol versions, or
// a firewall that drops the datagrams. None of them produce an error.
//
// Run it on BOTH machines and compare. Every line is chosen to be one somebody
// can act on, including the executable path — Windows firewall rules are keyed on
// it, and a machine typically has stale rules for half a dozen old copies.

#pragma once

#include <cstdint>

namespace hr::net {

// Prints the report. Returns a process exit code: 0 when nothing is obviously
// wrong, 1 when something here will stop the game being found.
int runNetDoctor(std::uint16_t gamePort);

}  // namespace hr::net
