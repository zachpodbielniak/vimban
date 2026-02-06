PREFIX ?= $(HOME)
BINDIR = $(PREFIX)/bin/scripts
SHAREDIR = $(PREFIX)/share/vimban
CONFIGDIR = $(HOME)/.config/vimban

.PHONY: help install uninstall test venv

help:
	@echo "vimban - Markdown-native ticket/kanban management system"
	@echo ""
	@echo "Targets:"
	@echo "  make install     Install vimban to $(PREFIX)"
	@echo "  make uninstall   Remove vimban installation"
	@echo "  make test        Verify dependencies are available"
	@echo "  make venv        Create Python virtual environment"
	@echo ""
	@echo "Configuration:"
	@echo "  PREFIX           Installation prefix (default: $(HOME))"
	@echo ""
	@echo "Examples:"
	@echo "  make install                    Install to ~/bin/scripts, ~/share/vimban"
	@echo "  make install PREFIX=/usr/local  Install system-wide"
	@echo ""

install:
	@echo "Installing vimban to $(PREFIX)..."
	mkdir -p $(BINDIR) $(SHAREDIR)/templates $(CONFIGDIR)
	cp bin/scripts/vimban bin/scripts/vimban_tui $(BINDIR)/
	cp bin/scripts/vimban_task bin/scripts/vimban_bug $(BINDIR)/
	cp bin/scripts/vimban_epic bin/scripts/vimban_story $(BINDIR)/
	cp bin/scripts/vimban_subtask bin/scripts/vimban_research $(BINDIR)/
	cp bin/scripts/vimban_area bin/scripts/vimban_resource $(BINDIR)/
	cp bin/scripts/vimban_meeting bin/scripts/vimban_journal $(BINDIR)/
	cp bin/scripts/vimban_recipe bin/scripts/vimban_mentor $(BINDIR)/
	chmod +x $(BINDIR)/vimban*
	cp share/vimban/templates/*.md $(SHAREDIR)/templates/
	@if [ ! -f $(CONFIGDIR)/config.yaml ]; then \
		cp .config/vimban/config.yaml.example $(CONFIGDIR)/config.yaml; \
		echo "Created default config at $(CONFIGDIR)/config.yaml"; \
	else \
		echo "Config already exists at $(CONFIGDIR)/config.yaml (not overwritten)"; \
	fi
	@echo ""
	@echo "Installation complete!"
	@echo "Ensure $(BINDIR) is in your PATH"
	@echo "Edit $(CONFIGDIR)/config.yaml to configure"

uninstall:
	@echo "Removing vimban from $(PREFIX)..."
	rm -f $(BINDIR)/vimban $(BINDIR)/vimban_tui
	rm -f $(BINDIR)/vimban_task $(BINDIR)/vimban_bug
	rm -f $(BINDIR)/vimban_epic $(BINDIR)/vimban_story
	rm -f $(BINDIR)/vimban_subtask $(BINDIR)/vimban_research
	rm -f $(BINDIR)/vimban_area $(BINDIR)/vimban_resource
	rm -f $(BINDIR)/vimban_meeting $(BINDIR)/vimban_journal
	rm -f $(BINDIR)/vimban_recipe $(BINDIR)/vimban_mentor
	rm -rf $(SHAREDIR)
	@echo "Uninstall complete (config at $(CONFIGDIR) preserved)"

test:
	@echo "Checking dependencies..."
	@command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 not found"; exit 1; }
	@python3 -c "import yaml" 2>/dev/null || { echo "FAIL: PyYAML not installed (pip install PyYAML)"; exit 1; }
	@python3 -c "import frontmatter" 2>/dev/null || { echo "FAIL: python-frontmatter not installed (pip install python-frontmatter)"; exit 1; }
	@command -v bash >/dev/null 2>&1 || { echo "FAIL: bash not found"; exit 1; }
	@command -v git >/dev/null 2>&1 || { echo "FAIL: git not found"; exit 1; }
	@echo "All required dependencies found!"
	@echo ""
	@echo "Optional dependencies:"
	@python3 -c "import mcp" 2>/dev/null && echo "  mcp: found" || echo "  mcp: not found (optional, for MCP server)"
	@python3 -c "import mistune" 2>/dev/null && echo "  mistune: found" || echo "  mistune: not found (optional, for TUI markdown rendering)"

venv:
	python3 -m venv venv
	./venv/bin/pip install -r requirements.txt
	@echo ""
	@echo "Virtual environment created in ./venv"
	@echo "Activate with: source venv/bin/activate"
