use std::collections::BTreeMap;
use std::ffi::{CString, c_char};
use std::io::Write;
use std::process::exit;

pub const SERVER_READ_BUFFER: usize = 64 * 1024;
pub const UDP_SOCKET_BUFFER: usize = 4 * 1024 * 1024;
pub const UDP_QUIET_NS: i64 = 200_000_000;

pub struct Args {
    values: BTreeMap<String, String>,
}

impl Args {
    pub fn parse(arguments: &[String]) -> Args {
        let mut values = BTreeMap::new();
        for argument in arguments {
            match argument.find('=') {
                Some(eq) if eq > 0 => {
                    values
                        .entry(argument[..eq].to_string())
                        .or_insert_with(|| argument[eq + 1..].to_string());
                }
                _ => {
                    eprintln!("bad argument '{argument}': expected key=value");
                    exit(1);
                }
            }
        }
        Args { values }
    }

    pub fn text(&self, key: &str) -> &str {
        match self.values.get(key) {
            Some(value) => value,
            None => {
                eprintln!("missing parameter '{key}'");
                exit(1);
            }
        }
    }

    pub fn integer(&self, key: &str) -> i64 {
        let value = self.text(key);
        let parsed = with_c_text(value, |start, end| unsafe { libc::strtoll(start, end, 10) });
        parsed.unwrap_or_else(|| {
            eprintln!("parameter '{key}' is not an integer: {value}");
            exit(1);
        })
    }

    pub fn real(&self, key: &str) -> f64 {
        let value = self.text(key);
        let parsed = with_c_text(value, |start, end| unsafe { libc::strtod(start, end) });
        parsed.unwrap_or_else(|| {
            eprintln!("parameter '{key}' is not a number: {value}");
            exit(1);
        })
    }

    pub fn all(&self) -> &BTreeMap<String, String> {
        &self.values
    }
}

fn with_c_text<T>(value: &str, parse: impl FnOnce(*const c_char, *mut *mut c_char) -> T) -> Option<T> {
    let text = CString::new(value).ok()?;
    let start = text.as_ptr();
    let mut end: *mut c_char = std::ptr::null_mut();
    let parsed = parse(start, &mut end);
    let consumed_nothing = std::ptr::eq(end.cast_const(), start);
    let trailing = !end.is_null() && unsafe { *end } != 0;
    if consumed_nothing || trailing { None } else { Some(parsed) }
}

pub fn now_ns() -> i64 {
    let mut now = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut now) };
    now.tv_sec * 1_000_000_000 + now.tv_nsec
}

pub fn process_cpu_s() -> f64 {
    let mut usage: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut usage) };
    (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) as f64
        + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) as f64 / 1e6
}

pub fn warmup_ns(seconds: f64) -> i64 {
    ((seconds / 5.0).min(1.0) * 1e9) as i64
}

pub fn seconds_ns(seconds: f64) -> i64 {
    (seconds * 1e9) as i64
}

#[derive(Default)]
pub struct Latencies {
    ns: Vec<i64>,
}

impl Latencies {
    pub fn reserve(&mut self, n: usize) {
        self.ns.reserve(n);
    }

    pub fn add(&mut self, ns: i64) {
        self.ns.push(ns);
    }

    pub fn merge(&mut self, other: &Latencies) {
        self.ns.extend_from_slice(&other.ns);
    }

    pub fn percentile_us(&self, p: f64) -> f64 {
        if self.ns.is_empty() {
            return 0.0;
        }
        let mut copy = self.ns.clone();
        let index = (p / 100.0 * (copy.len() - 1) as f64 + 0.5) as usize;
        let (_, nth, _) = copy.select_nth_unstable(index);
        *nth as f64 / 1000.0
    }

    pub fn max_us(&self) -> f64 {
        self.ns.iter().max().map_or(0.0, |max| *max as f64 / 1000.0)
    }
}

pub enum Work {
    Count(i64),
    Text(String),
}

impl From<i64> for Work {
    fn from(count: i64) -> Work {
        Work::Count(count)
    }
}

impl From<String> for Work {
    fn from(text: String) -> Work {
        Work::Text(text)
    }
}

#[derive(Default)]
pub struct Report {
    pub metrics: BTreeMap<String, f64>,
    pub work: BTreeMap<String, Work>,
}

impl Report {
    pub fn metric(&mut self, name: &str, value: f64) {
        self.metrics.insert(name.to_string(), value);
    }

    pub fn work(&mut self, name: &str, value: impl Into<Work>) {
        self.work.insert(name.to_string(), value.into());
    }
}

pub struct Xorshift64 {
    x: u64,
}

impl Default for Xorshift64 {
    fn default() -> Xorshift64 {
        Xorshift64 { x: 42 }
    }
}

impl Xorshift64 {
    pub fn next(&mut self) -> u64 {
        self.x ^= self.x << 13;
        self.x ^= self.x >> 7;
        self.x ^= self.x << 17;
        self.x
    }
}

pub fn unsupported(why: &str) -> ! {
    eprintln!("unsupported: {why}");
    exit(2);
}

pub fn fail(why: &str) -> ! {
    eprintln!("failed: {why}");
    exit(1);
}

pub fn check(condition: bool, why: &str) {
    if !condition {
        fail(why);
    }
}

pub fn cert_dir() -> String {
    match std::env::var_os("BENCH_CERT_DIR") {
        Some(dir) => dir.to_string_lossy().into_owned(),
        None => concat!(env!("CARGO_MANIFEST_DIR"), "/../certs").to_string(),
    }
}

pub fn server_cert_path() -> String {
    cert_dir() + "/server.pem"
}

pub fn server_key_path() -> String {
    cert_dir() + "/server.key"
}

pub fn ca_cert_path() -> String {
    cert_dir() + "/ca.pem"
}

pub fn payload(size: usize) -> Vec<u8> {
    (0..size).map(|i| i.wrapping_mul(131).wrapping_add(7) as u8).collect()
}

pub fn json_string(text: &str) -> String {
    let mut out = String::from("\"");
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

pub fn shortest_number(value: f64) -> String {
    if value == 0.0 {
        return if value.is_sign_negative() { "-0" } else { "0" }.to_string();
    }
    let exponential = format!("{:e}", value.abs());
    let (mantissa, exponent) = exponential.split_once('e').expect("{:e} always has an exponent");
    let exponent: i64 = exponent.parse().expect("{:e} exponents are integers");
    let digits: String = mantissa.chars().filter(char::is_ascii_digit).collect();
    let sign = if value < 0.0 { "-" } else { "" };

    let mut scientific = format!("{sign}{}", &digits[..1]);
    if digits.len() > 1 {
        scientific.push('.');
        scientific.push_str(&digits[1..]);
    }
    scientific.push_str(&format!("e{}{:02}", if exponent < 0 { '-' } else { '+' }, exponent.abs()));

    let integer_digits = exponent + 1;
    let count = digits.len() as i64;
    let fixed = if integer_digits <= 0 {
        format!("{sign}0.{}{digits}", "0".repeat((-integer_digits) as usize))
    } else if integer_digits >= count {
        format!("{sign}{digits}{}", "0".repeat((integer_digits - count) as usize))
    } else {
        let split = integer_digits as usize;
        format!("{sign}{}.{}", &digits[..split], &digits[split..])
    };

    if fixed.len() <= scientific.len() { fixed } else { scientific }
}

pub type Workload = fn(&Args) -> Report;

pub fn run(library: &str, version: &str, workloads: &BTreeMap<&str, Workload>) -> i32 {
    let argv: Vec<String> = std::env::args_os().map(|a| a.to_string_lossy().into_owned()).collect();
    if argv.len() < 2 {
        let program = argv.first().map_or("bench", String::as_str);
        let mut usage = format!("usage: {program} <workload> key=value ...\nworkloads:");
        for name in workloads.keys() {
            usage.push(' ');
            usage.push_str(name);
        }
        eprintln!("{usage}");
        return 1;
    }
    let Some(workload) = workloads.get(argv[1].as_str()) else {
        unsupported(&format!("{library} has no workload '{}'", argv[1]));
    };

    let args = Args::parse(&argv[2..]);
    let report = workload(&args);

    let variant = std::env::var_os("BENCH_VARIANT").map_or_else(|| "default".to_string(), |v| v.to_string_lossy().into_owned());
    let params: Vec<String> = args
        .all()
        .iter()
        .map(|(key, value)| format!("{}:{}", json_string(key), json_string(value)))
        .collect();
    let metrics: Vec<String> = report
        .metrics
        .iter()
        .map(|(key, value)| {
            let number = if value.is_finite() { shortest_number(*value) } else { "null".to_string() };
            format!("{}:{number}", json_string(key))
        })
        .collect();
    let work: Vec<String> = report
        .work
        .iter()
        .map(|(key, value)| match value {
            Work::Count(count) => format!("{}:{count}", json_string(key)),
            Work::Text(text) => format!("{}:{}", json_string(key), json_string(text)),
        })
        .collect();
    let line = format!(
        "{{\"library\":{},\"library_version\":{},\"variant\":{},\"workload\":{},\"params\":{{{}}},\"metrics\":{{{}}},\"work\":{{{}}}}}",
        json_string(library),
        json_string(version),
        json_string(&variant),
        json_string(&argv[1]),
        params.join(","),
        metrics.join(","),
        work.join(",")
    );
    let mut stdout = std::io::stdout().lock();
    let _ = writeln!(stdout, "{line}").and_then(|()| stdout.flush());
    0
}
