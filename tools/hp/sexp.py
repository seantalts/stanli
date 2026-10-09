import re

_TOK = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+', re.S)


def parse(text):
    stack = [[]]
    for tok in _TOK.findall(text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(tok)
    return stack[0][0] if len(stack[0]) == 1 else stack[0]


def field(node, name):
    for item in node:
        if isinstance(item, list) and item and item[0] == name:
            return item[1] if len(item) == 2 else item[1:]
    raise KeyError(name)


def walk(node):
    yield node
    if isinstance(node, list):
        for child in node:
            yield from walk(child)
