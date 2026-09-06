"""Check the VS Code grammar against the compiler it is meant to describe.

The realistic failure for a hand-written TextMate grammar is not a
syntax error - it is a keyword nobody remembered to add, which shows up
as one word rendered in the wrong colour and is easy to never notice. So
the keyword list is read out of the lexer and every entry has to appear
somewhere in the grammar.
"""

import io
import json
import os
import re
import sys

# Run from anywhere: paths are relative to this file.
HERE = os.path.dirname(os.path.abspath(__file__))
EXT = HERE + '/'
REPO = os.path.normpath(HERE + '/../..') + '/'

problems = []

# --- every JSON file has to parse -------------------------------------
for rel in ('package.json', 'language-configuration.json',
            'syntaxes/soliton.tmLanguage.json', 'snippets/soliton.json'):
    path = EXT + rel
    try:
        json.load(io.open(path, encoding='utf-8'))
        print('parses  %s' % rel)
    except Exception as exc:
        problems.append('%s does not parse: %s' % (rel, exc))

grammar_text = io.open(EXT + 'syntaxes/soliton.tmLanguage.json', encoding='utf-8').read()
grammar = json.loads(grammar_text)

# --- every keyword the lexer knows must appear ------------------------
lexer = io.open(REPO + 'libs/lexer/src/token.cpp', encoding='utf-8').read()
table = lexer[lexer.index('kKeywords{{'):]
table = table[:table.index('}};')]
keywords = re.findall(r'\{"([a-z]+)"', table)
print('\nlexer keywords: %d' % len(keywords))

for kw in keywords:
    if not re.search(r'\b%s\b' % re.escape(kw), grammar_text):
        problems.append('keyword `%s` appears nowhere in the grammar' % kw)

# --- and every intrinsic ----------------------------------------------
typeck = io.open(REPO + 'libs/typeck/src/typeck.cpp', encoding='utf-8').read()
block = typeck[typeck.index('kIntrinsics{'):]
block = block[:block.index('};')]
intrinsics = re.findall(r'"([a-z_]+)"', block)
print('intrinsics:     %d' % len(intrinsics))

for name in intrinsics:
    if not re.search(r'\b%s\b' % re.escape(name), grammar_text):
        problems.append('intrinsic `%s` is not highlighted' % name)

# --- the extensions the manifest claims must match the compiler -------
ast = io.open(REPO + 'libs/ast/include/soliton/ast/ast.hpp', encoding='utf-8').read()
iface = io.open(REPO + 'libs/ast/include/soliton/ast/interface.hpp', encoding='utf-8').read()
src_ext = re.search(r'kFileExtension = "(\w+)"', ast).group(1)
int_ext = re.search(r'kInterfaceExtension = "(\w+)"', iface).group(1)

manifest = json.load(io.open(EXT + 'package.json', encoding='utf-8'))
declared = manifest['contributes']['languages'][0]['extensions']
for want in ('.' + src_ext, '.' + int_ext):
    if want not in declared:
        problems.append('manifest does not claim %s (compiler uses it)' % want)
print('extensions:     %s' % ', '.join(declared))

# --- files the manifest points at must exist --------------------------
for rel in (manifest['contributes']['languages'][0]['configuration'],
            manifest['contributes']['grammars'][0]['path'],
            manifest['contributes']['snippets'][0]['path'],
            manifest['contributes']['languages'][0]['icon']['dark']):
    target = os.path.normpath(EXT + rel.lstrip('./'))
    if not os.path.exists(target):
        problems.append('manifest points at %s, which does not exist' % rel)

icon = manifest.get('icon')
if icon and not os.path.exists(EXT + icon):
    problems.append('package icon %s does not exist' % icon)

print()
if problems:
    for p in problems:
        print('PROBLEM  ' + p)
    sys.exit(1)
print('grammar agrees with the compiler')
