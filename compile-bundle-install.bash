#!/usr/bin/env bash

make package
code --uninstall-extension uo-june98.wombat-ext
code --install-extension ./wombat-ext-0.1.0.vsix

