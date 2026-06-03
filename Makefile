
.PHONY: all compiler extension package clean

# Build everything: C compiler first, then the VS Code extension
all: compiler extension

# Build the wombat-compiler C binary and copy it to bin/
compiler:
	$(MAKE) -C compiler

# Compile TypeScript (client + LSP server). server/npm install is handled by the compile script.
extension:
	npm install
	npm run compile

# Create the .vsix package (runs bundle + vsce package)
package: compiler extension
	npm run package

clean:
	$(MAKE) -C compiler clean
	rm -rf out/ server/out/ bin/wombat-compiler
