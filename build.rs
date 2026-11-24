use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=app.manifest");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=CARGO_PKG_VERSION");

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let version = env::var("CARGO_PKG_VERSION").unwrap_or_else(|_| "0.0.0".to_string());

    let mut parts: Vec<u32> = version
        .split('.')
        .map(|p| {
            p.chars()
                .take_while(|c| c.is_ascii_digit())
                .collect::<String>()
                .parse::<u32>()
                .unwrap_or(0)
        })
        .collect();
    while parts.len() < 4 {
        parts.push(0);
    }
    let numeric = format!("{},{},{},{}", parts[0], parts[1], parts[2], parts[3]);
    let version4 = format!("{}.{}.{}.{}", parts[0], parts[1], parts[2], parts[3]);

    let manifest_rc = manifest_dir
        .join("app.manifest")
        .to_string_lossy()
        .replace('\\', "\\\\");

    let mut text = String::new();
    text.push_str("#pragma code_page(65001)\n");
    text.push_str(&format!("1 24 \"{}\"\n", manifest_rc));
    text.push_str(&format!(
        "1 VERSIONINFO\nFILEVERSION {numeric}\nPRODUCTVERSION {numeric}\nFILEFLAGSMASK 0x3fL\nFILEFLAGS 0x0L\nFILEOS 0x40004L\nFILETYPE 0x1L\nFILESUBTYPE 0x0L\nBEGIN\n    BLOCK \"StringFileInfo\"\n    BEGIN\n        BLOCK \"040904b0\"\n        BEGIN\n"
    ));
    for (key, value) in [
        ("CompanyName", "NetConditioner"),
        ("FileDescription", "NetConditioner"),
        ("FileVersion", version4.as_str()),
        ("InternalName", "NetConditioner"),
        ("OriginalFilename", "NetConditioner.exe"),
        ("ProductName", "NetConditioner"),
        ("ProductVersion", version.as_str()),
    ] {
        text.push_str(&format!("            VALUE \"{}\", \"{}\"\n", key, value));
    }
    text.push_str(
        "        END\n    END\n    BLOCK \"VarFileInfo\"\n    BEGIN\n        VALUE \"Translation\", 0x409, 1200\n    END\nEND\n",
    );

    let rc_path = out_dir.join("netconditioner.rc");
    fs::write(&rc_path, text).expect("write resource script");

    embed_resource::compile(&rc_path, embed_resource::NONE)
        .manifest_required()
        .unwrap_or_else(|e| {
            panic!("编译 Windows 资源失败（需要 Windows SDK 的 rc.exe 与 MT.exe）：{e}");
        });
}
