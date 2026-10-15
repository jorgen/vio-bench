use std::path::Path;

fn locked_version(lock: &str, package: &str) -> Option<String> {
    let entry = format!("name = \"{package}\"\nversion = \"");
    let start = lock.find(&entry)? + entry.len();
    let length = lock[start..].find('"')?;
    Some(lock[start..start + length].to_string())
}

fn main() {
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").expect("cargo sets CARGO_MANIFEST_DIR");
    let lock_path = Path::new(&manifest_dir).join("Cargo.lock");
    println!("cargo:rerun-if-changed={}", lock_path.display());
    let lock = std::fs::read_to_string(&lock_path).expect("Cargo.lock exists once cargo has resolved the dependencies");
    for (package, variable) in [("tokio", "BENCH_TOKIO_VERSION"), ("rustls", "BENCH_RUSTLS_VERSION")] {
        let version = locked_version(&lock, package).unwrap_or_else(|| panic!("{package} is not in Cargo.lock"));
        println!("cargo:rustc-env={variable}={version}");
    }
}
