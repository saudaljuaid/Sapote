// SPDX-License-Identifier: GPL-3.0-only
//! Golden output for the complete `SapStudio` slate (R-14.4).
//!
//! The transcript pins timecode, project and reel formats, render output, and
//! their digests. The selected frame includes a half-opacity layer, so the
//! expected red value also checks linear-light compositing.

use sapstudio_abi::seam::{Console, Result, SeamStatus};
use sapstudio_app::{EXIT_FAILURE, EXIT_SUCCESS, run};

/// What the slate must print, byte for byte.
const GOLDEN: &str = "\
SapStudio slate

timebase       30000/1001
media length   00:10:00;00

after the cut
  V1  00:00:00;00  00:00:58;10  clip  src 00:00:11;18
  V1  00:00:58;10  00:01:28;12  clip  src 00:02:46;24
  A1  00:00:00;00  00:01:40;00  clip  src 00:00:10;00
duration       00:01:40;00

undone         7 edits
tracks now     0
redone         7 edits
restored       true

saved          317
digest         D6245B0168B3C9EF53D15D0AAC948B7FABEE496F77E49583FDE65E69F15EC449
round trip     true

reel frames    3
reel bytes     528
reel digest    53F707D400833A4F2492DCC4A819B30C78367758C69FEA8E68AF28FB7A85D800
reel matches   true
pool frames    2
pool bytes     288
pool evictions 1

picture size   576
picture digest 74202C026AAA084C24490F8ED394BF021782A396882F2632B51949E8A20F9D20
picture red    98
picture alpha  255

slate complete
";

/// A console that keeps what it was given, so a test can read it back.
#[derive(Default)]
struct Buffer(std::vec::Vec<u8>);

impl Console for Buffer {
    fn write(&mut self, bytes: &[u8]) -> Result<()> {
        self.0.extend_from_slice(bytes);
        Ok(())
    }
}

/// A console that refuses after a given number of calls.
struct Failing {
    remaining: usize,
}

impl Console for Failing {
    fn write(&mut self, _: &[u8]) -> Result<()> {
        if self.remaining == 0 {
            return Err(SeamStatus::Refused);
        }
        self.remaining -= 1;
        Ok(())
    }
}

#[test]
fn the_slate_prints_its_golden_transcript() {
    let mut buffer = Buffer::default();
    assert_eq!(run(&mut buffer), EXIT_SUCCESS);
    let transcript = std::string::String::from_utf8(buffer.0).expect("the report is text");
    assert_eq!(transcript, GOLDEN);
}

#[test]
fn the_slate_is_deterministic() {
    // R-4.1 in the smallest form the application currently has: the same run
    // twice produces the same bytes. Nothing in it may consult a clock, an
    // address, or an allocation order.
    let mut first = Buffer::default();
    let mut second = Buffer::default();
    assert_eq!(run(&mut first), EXIT_SUCCESS);
    assert_eq!(run(&mut second), EXIT_SUCCESS);
    assert_eq!(first.0, second.0);
}

#[test]
fn a_console_refusal_is_reported_rather_than_ignored() {
    // Formatting cannot carry a seam refusal out through core::fmt, so the
    // writer records it. If that plumbing were dropped, the slate would
    // silently claim success on a console that wrote nothing.
    let mut failing = Failing { remaining: 3 };
    assert_eq!(run(&mut failing), EXIT_FAILURE);
}
