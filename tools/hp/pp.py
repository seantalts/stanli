import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from sexp import parse, field


def simp(n):
    if isinstance(n, str):
        return n
    if len(n) == 2 and isinstance(n[0], list) and n[0][0] == 'pattern':
        return simp(n[0][1])
    if len(n) == 2 and n[0] == 'pattern':
        return simp(n[1])
    out = []
    for c in n:
        if isinstance(c, list) and c and c[0] in ('meta', 'loc'):
            continue
        out.append(simp(c))
    return out


def show(n, ind=0):
    s = fmt(n)
    if len(s) + ind < 110 or isinstance(n, str):
        return ' ' * ind + s
    if isinstance(n, list) and n and isinstance(n[0], str):
        return ' ' * ind + '(' + n[0] + '\n' + '\n'.join(show(c, ind + 2) for c in n[1:]) + ')'
    return ' ' * ind + '(\n' + '\n'.join(show(c, ind + 2) for c in n) + ')'


def fmt(n):
    if isinstance(n, str):
        return n
    return '(' + ' '.join(fmt(c) for c in n) + ')'


if __name__ == '__main__':
    t = parse(open(sys.argv[1]).read())
    for sec in sys.argv[2:]:
        print('==', sec)
        for s in field(t, sec):
            print(show(simp(s)))
