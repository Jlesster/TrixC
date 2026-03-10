package tree_sitter_trixlang_test

import (
	"testing"

	tree_sitter "github.com/tree-sitter/go-tree-sitter"
	tree_sitter_trixlang "github.com/jlesster/trixc.git/bindings/go"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_trixlang.Language())
	if language == nil {
		t.Errorf("Error loading Trix-Lang grammar")
	}
}
