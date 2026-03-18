import XCTest
import SwiftTreeSitter
import TreeSitterTrixlang

final class TreeSitterTrixlangTests: XCTestCase {
    func testCanLoadGrammar() throws {
        let parser = Parser()
        let language = Language(language: tree_sitter_trixlang())
        XCTAssertNoThrow(try parser.setLanguage(language),
                         "Error loading Trix-Lang grammar")
    }
}
