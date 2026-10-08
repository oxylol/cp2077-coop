// Type-checks the files listed in COOP_REDS (separated by ';') with redscript's frontend and prints diagnostics.
use std::env;

use redscript_ast::SourceMap;
use redscript_compiler_api::{CompileErrorReporter, SourceMapExt, Symbols, TypeInterner};
use redscript_compiler_frontend::infer_from_sources;

#[test]
fn coop_check() {
    let files: Vec<String> = env::var("COOP_REDS")
        .expect("COOP_REDS not set")
        .split(';')
        .map(str::to_owned)
        .collect();
    let sources = SourceMap::from_files(&files).unwrap();
    sources.populate_boot_lib();

    let interner = TypeInterner::default();
    let symbols = Symbols::with_default_types();
    let mut reporter = CompileErrorReporter::default();

    let (_, _) = infer_from_sources(&sources, symbols, &mut reporter, &interner);
    let reported = reporter.into_reported();
    for diagnostic in &reported {
        println!("{}", diagnostic.display(&sources).unwrap());
    }
    println!("COOP_DIAGNOSTICS {}", reported.len());
}
