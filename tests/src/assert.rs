use std::time::Duration;
use vortex::{ConsoleOutput, ConsoleStream};

// ── Collected-output assertions ─────────────────────────────────────────────

pub fn assert_subsystem_ok(out: &ConsoleOutput, subsystem: &str) {
    let prefix = format!("[{}] OK", subsystem);
    if !out.kernel_contains(&prefix) {
        panic!(
            "assert_subsystem_ok({:?}): not found in kernel output\n\nCapture:\n{}",
            subsystem,
            out.kernel_lines.join("\n")
        );
    }
}

pub fn assert_subsystem_fail(out: &ConsoleOutput, subsystem: &str) {
    let prefix = format!("[{}] FAIL", subsystem);
    if !out.kernel_contains(&prefix) {
        panic!(
            "assert_subsystem_fail({:?}): not found in kernel output\n\nCapture:\n{}",
            subsystem,
            out.kernel_lines.join("\n")
        );
    }
}

pub fn assert_boot_subsequence(out: &ConsoleOutput, expected: &[&str]) {
    let mut matched = 0;
    for line in &out.kernel_lines {
        if matched == expected.len() {
            break;
        }
        if line.contains(expected[matched]) {
            matched += 1;
        }
    }
    if matched < expected.len() {
        panic!(
            "assert_boot_subsequence: matched {}/{} lines; first missing: {:?}\n\nCapture:\n{}",
            matched,
            expected.len(),
            expected[matched],
            out.kernel_lines.join("\n")
        );
    }
}

pub fn assert_line_contains(out: &ConsoleOutput, substr: &str) {
    if !out.contains(substr) {
        panic!(
            "assert_line_contains({:?}): not found in output\n\nCapture:\n{}",
            substr,
            out.lines.join("\n")
        );
    }
}

pub fn assert_no_line_contains(out: &ConsoleOutput, substr: &str) {
    if let Some(line) = out.lines.iter().find(|l| l.contains(substr)) {
        panic!(
            "assert_no_line_contains({:?}): found unexpected line: {:?}\n\nCapture:\n{}",
            substr,
            line,
            out.lines.join("\n")
        );
    }
}

// ── Streaming assertion ──────────────────────────────────────────────────────

#[derive(Debug)]
pub struct WaitTimeout {
    pub pattern: String,
}

impl std::fmt::Display for WaitTimeout {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "wait_for_line: timed out waiting for {:?}", self.pattern)
    }
}

pub async fn wait_for_line(
    stream: &mut ConsoleStream,
    pattern: &str,
    timeout: Duration,
) -> Result<String, WaitTimeout> {
    let deadline = tokio::time::Instant::now() + timeout;
    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) if line.contains(pattern) => return Ok(line),
            Ok(Some(_)) => continue,
            Ok(None) | Err(_) => {
                return Err(WaitTimeout {
                    pattern: pattern.to_string(),
                })
            }
        }
    }
}

// ── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn out(kernel_lines: &[&str]) -> ConsoleOutput {
        ConsoleOutput {
            lines: kernel_lines.iter().map(|s| s.to_string()).collect(),
            kernel_lines: kernel_lines.iter().map(|s| s.to_string()).collect(),
        }
    }

    // assert_subsystem_ok

    #[test]
    fn subsystem_ok_passes_when_present() {
        let o = out(&["[PMM] OK: 512MB mapped", "[VMM] OK: paging"]);
        assert_subsystem_ok(&o, "PMM"); // must not panic
    }

    #[test]
    #[should_panic(expected = "assert_subsystem_ok(\"PMM\")")]
    fn subsystem_ok_panics_when_absent() {
        let o = out(&["[VMM] OK: paging"]);
        assert_subsystem_ok(&o, "PMM");
    }

    // assert_subsystem_fail

    #[test]
    fn subsystem_fail_passes_when_present() {
        let o = out(&["[EXT2] FAIL: bad magic"]);
        assert_subsystem_fail(&o, "EXT2");
    }

    #[test]
    #[should_panic(expected = "assert_subsystem_fail(\"EXT2\")")]
    fn subsystem_fail_panics_when_absent() {
        let o = out(&["[EXT2] OK: mounted"]);
        assert_subsystem_fail(&o, "EXT2");
    }

    // assert_boot_subsequence

    #[test]
    fn subsequence_passes_exact() {
        let o = out(&["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
    }

    #[test]
    fn subsequence_passes_with_gaps() {
        let o = out(&["[PMM] OK", "[NOISE]", "[VMM] OK", "[MORE NOISE]", "[SCHED] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
    }

    #[test]
    #[should_panic(expected = "assert_boot_subsequence")]
    fn subsequence_fails_wrong_order() {
        let o = out(&["[VMM] OK", "[PMM] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK"]);
    }

    #[test]
    #[should_panic(expected = "assert_boot_subsequence")]
    fn subsequence_fails_missing_line() {
        let o = out(&["[PMM] OK", "[VMM] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
    }

    // assert_line_contains

    #[test]
    fn line_contains_passes() {
        let o = out(&["[NET] configured: 10.0.2.15"]);
        assert_line_contains(&o, "[NET] configured:");
    }

    #[test]
    #[should_panic(expected = "assert_line_contains")]
    fn line_contains_panics_when_absent() {
        let o = out(&["[PMM] OK"]);
        assert_line_contains(&o, "[NET] configured:");
    }

    // assert_no_line_contains

    #[test]
    fn no_line_contains_passes_when_absent() {
        let o = out(&["[PMM] OK", "[VMM] OK"]);
        assert_no_line_contains(&o, "[NET]");
    }

    #[test]
    #[should_panic(expected = "assert_no_line_contains")]
    fn no_line_contains_panics_when_present() {
        let o = out(&["[PMM] OK", "[NET] configured: 10.0.2.15"]);
        assert_no_line_contains(&o, "[NET]");
    }
}
