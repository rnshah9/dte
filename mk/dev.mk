DIST_VERSIONS = 1.6 1.5 1.4 1.3 1.2 1.1 1.0
DIST_ALL = $(addprefix dte-, $(addsuffix .tar.gz, $(DIST_VERSIONS)))
GIT_HOOKS = $(addprefix .git/hooks/, commit-msg pre-commit)
SYNTAX_LINT = $(AWK) -f tools/syntax-lint.awk

dist: $(firstword $(DIST_ALL))
dist-all: $(DIST_ALL)
git-hooks: $(GIT_HOOKS)

check-syntax-files:
	$(E) LINT 'config/syntax/*'
	$(Q) $(SYNTAX_LINT) $(addprefix config/syntax/, $(BUILTIN_SYNTAX_FILES))

check-dist: dist-all
	@sha256sum -c mk/sha256sums.txt

$(DIST_ALL): dte-%.tar.gz:
	$(E) ARCHIVE $@
	$(Q) git archive --prefix='dte-$*/' -o '$@' 'v$*'

$(GIT_HOOKS): .git/hooks/%: tools/git-hooks/%
	$(E) CP $@
	$(Q) cp $< $@


CLEANFILES += dte-*.tar.gz
.PHONY: dist dist-all check-dist check-syntax-files git-hooks
