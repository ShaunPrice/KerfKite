"""Bounded parser for KerfKite's OpenSCAD-style constructive geometry language.

Lengths are in mm and angles are in degrees. Evaluation returns data for the geometry
kernel. The language has no imports, filesystem operations or host-language calls.
"""
from dataclasses import dataclass, field
import math
import operator
import re


MAX_SOURCE = 65536
MAX_TOKENS = 16000
MAX_DEPTH = 48
MAX_RANGE = 1024
MAX_VECTOR_SIZE = 16384
MAX_GEOMETRY = 1500
MAX_WORK = 180000
MAX_NUMBER = 1e9

GEOMETRY = frozenset((
    'square', 'circle', 'polygon', 'cube', 'sphere', 'cylinder', 'union', 'difference',
    'intersection', 'hull', 'translate', 'rotate', 'scale', 'mirror', 'linear_extrude',
    'rotate_extrude', 'offset', 'projection', 'polyhedron', 'multmatrix',
))
FACET_OPERATIONS = {'circle', 'sphere', 'cylinder', 'rotate_extrude', 'offset'}
RESERVED = {'true', 'false', 'PI', '$children', 'if', 'else', 'module', 'function', 'for', 'children'}
TOKEN = re.compile(
    r'(?P<skip>\s+|//[^\n]*|/\*[\s\S]*?\*/)|(?P<comment>/\*)|'
    r'(?P<number>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?)|'
    r'(?P<name>\$?[A-Za-z_][A-Za-z0-9_]*)|'
    r'(?P<symbol><=|>=|==|!=|&&|\|\||[{}()\[\],;:+\-*/%^=!<>?])|(?P<invalid>.)'
)
IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*\Z')


class ScadError(ValueError):
    """A diagnostic suitable for showing beside the source editor."""

    def __init__(self, token, message):
        self.line = token.line
        self.column = token.column
        super().__init__(f'Line {self.line}, column {self.column}: {message}')


@dataclass(frozen=True)
class Token:
    text: str
    line: int
    column: int
    kind: str = ''


def tokenize(source):
    if not isinstance(source, str) or len(source) > MAX_SOURCE or len(source.encode('utf-8', 'replace')) > MAX_SOURCE:
        raise ScadError(Token('', 1, 1), f'Source must be text of at most {MAX_SOURCE} UTF-8 bytes')
    result = []
    line, column = 1, 1
    for match in TOKEN.finditer(source):
        text = match[0]
        kind = match.lastgroup
        token = Token(text, line, column, kind)
        if kind == 'name' and text in ('include', 'use', 'import'):
            raise ScadError(token, f'External file command {text!r} is unsupported')
        if kind == 'comment':
            raise ScadError(token, 'Unterminated block comment')
        if kind == 'invalid':
            raise ScadError(token, f'Unsupported character {text!r}')
        if kind != 'skip':
            result.append(token)
            if len(result) > MAX_TOKENS:
                raise ScadError(token, 'Source has too many tokens')
        breaks = text.count('\n')
        if breaks:
            line += breaks
            column = len(text) - text.rfind('\n')
        else:
            column += len(text)
    return [*result, Token('', line, column, 'end')]


@dataclass(frozen=True)
class Node:
    token: Token
    kind: str
    parts: tuple = ()


PRECEDENCE = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '<=': 4, '>': 4,
              '>=': 4, '+': 5, '-': 5, '*': 6, '/': 6, '%': 6, '^': 8}


class Parser:
    def __init__(self, source):
        self.tokens = tokenize(source)
        self.at = 0

    @property
    def token(self):
        return self.tokens[self.at]

    def take(self):
        token = self.token
        self.at += 1
        return token

    def accept(self, spelling):
        if self.token.text == spelling:
            self.take()
            return True
        return False

    def expect(self, spelling):
        if not self.accept(spelling):
            raise ScadError(self.token, f'Expected {spelling!r}')

    def name(self, binding=False):
        token = self.token
        if token.kind != 'name':
            raise ScadError(token, 'Expected a name')
        if binding and (token.text in RESERVED or token.text.startswith('$') and token.text != '$fn'):
            raise ScadError(token, f'Cannot bind {token.text!r}')
        self.take()
        return token

    def depth(self, level):
        if level >= MAX_DEPTH:
            raise ScadError(self.token, 'Source is nested too deeply')

    def program(self, level=0, close=''):
        statements = []
        while self.token.text != close:
            if self.token.kind == 'end':
                raise ScadError(self.token, f'Expected {close!r}')
            statements.append(self.statement(level))
        if close:
            self.take()
        return statements

    def arguments(self, level):
        self.expect('(')
        args, named = [], set()
        if not self.accept(')'):
            while True:
                name = None
                if self.token.kind == 'name' and self.tokens[self.at + 1].text == '=':
                    name = self.take()
                    self.take()
                    if name.text in named:
                        raise ScadError(name, f'Duplicate argument {name.text!r}')
                    named.add(name.text)
                elif named:
                    raise ScadError(self.token, 'Positional arguments must precede named arguments')
                args.append((name.text if name else None, self.expression(level+1)))
                if self.accept(')'):
                    break
                self.expect(',')
        return args

    def bindings(self, level, defaults, end=')'):
        items, names = [], set()
        if defaults and self.accept(end):
            return items
        while True:
            token = self.name(binding=True)
            if token.text in names:
                raise ScadError(token, f'Duplicate binding {token.text!r}')
            names.add(token.text)
            expr = None
            if self.accept('='):
                expr = self.expression(level+1)
            elif not defaults:
                raise ScadError(self.token, 'Expected an initializer')
            items.append((token.text, expr))
            if self.accept(end):
                return items
            self.expect(',')

    def statement(self, level):
        self.depth(level)
        t = self.token
        if self.accept(';'):
            return Node(t, 'block', ([],))
        if self.accept('{'):
            return Node(t, 'block', (self.program(level+1, '}'),))
        if t.text in ('module', 'function'):
            self.take()
            name = self.name(binding=True)
            self.expect('(')
            params = self.bindings(level, True)
            if t.text == 'function':
                self.expect('=')
                body = self.expression(level+1)
                self.expect(';')
            else:
                body = self.statement(level+1)
            return Node(t, t.text, (name.text, params, body))
        if self.accept('if'):
            self.expect('(')
            condition = self.expression(level+1)
            self.expect(')')
            yes = self.statement(level+1)
            no = self.statement(level+1) if self.accept('else') else None
            return Node(t, 'if', (condition, yes, no))
        if self.accept('for'):
            self.expect('(')
            bindings = self.bindings(level, False)
            return Node(t, 'for', (bindings, self.statement(level+1)))
        if t.kind == 'name' and self.tokens[self.at+1].text == '=':
            self.name(binding=True)
            self.take()
            expr = self.expression(level+1)
            self.expect(';')
            return Node(t, 'assign', (t.text, expr))
        self.name()
        args = self.arguments(level)
        if self.accept(';'):
            children = []
        elif self.accept('{'):
            children = self.program(level+1, '}')
        else:
            children = [self.statement(level+1)]
        return Node(t, 'call', (t.text, args, children))

    def expression(self, level, minimum=0):
        self.depth(level)
        t = self.token
        if self.accept('('):
            left = self.expression(level+1)
            self.expect(')')
        elif self.accept('['):
            left = self.vector(t, level)
        elif t.text in ('+', '-', '!'):
            self.take()
            left = Node(t, 'unary', (t.text, self.expression(level+1, 7)))
        elif t.kind == 'number':
            self.take()
            left = Node(t, 'literal', (number(float(t.text), t),))
        elif t.kind == 'name':
            self.take()
            if self.token.text == '(':
                left = Node(t, 'function_call', (t.text, self.arguments(level),))
            else:
                left = Node(t, 'var', (t.text,))
        else:
            raise ScadError(t, 'Expected a number, variable, vector or expression')
        while True:
            token = self.token
            if self.accept('['):
                left = Node(token, 'index', (left, self.expression(level+1)))
                self.expect(']')
            elif token.text in PRECEDENCE and PRECEDENCE[token.text] >= minimum:
                self.take()
                precedence = PRECEDENCE[token.text] + (token.text != '^')
                left = Node(token, 'binary', (token.text, left, self.expression(level+1, precedence)))
            else:
                break
        if minimum == 0 and self.accept('?'):
            yes = self.expression(level+1)
            self.expect(':')
            left = Node(t, 'ternary', (left, yes, self.expression(level+1)))
        return left

    def vector(self, token, level):
        if self.accept(']'):
            return Node(token, 'vector', ([],))
        if self.accept('for'):
            self.expect('(')
            bindings = self.bindings(level, False)
            value = self.expression(level+1)
            self.expect(']')
            return Node(token, 'comprehension', (bindings, value))
        first = self.expression(level+1)
        if self.accept(':'):
            middle = self.expression(level+1)
            end = self.expression(level+1) if self.accept(':') else None
            self.expect(']')
            return Node(token, 'range', (first, middle, end))
        items = [first]
        while self.accept(',') and self.token.text != ']':
            items.append(self.expression(level+1))
        self.expect(']')
        return Node(token, 'vector', (items,))


def number(value, token):
    if type(value) not in (float, int) or not -MAX_NUMBER <= value <= MAX_NUMBER:
        raise ScadError(token, f'Expected a finite number between {-MAX_NUMBER:g} and {MAX_NUMBER:g}')
    return value


def index_number(value, length, token):
    value = number(value, token)
    if int(value) != value or value < 0 or value >= length:
        raise ScadError(token, 'Index outside vector bounds')
    return int(value)


class Vector(list):
    def __init__(self, items):
        super().__init__(items)
        self.cost = 1 + sum(value_cost(v) for v in self)
        self.depth = 1 + max((v.depth for v in self if isinstance(v, Vector)), default=0)


def value_cost(value):
    return value.cost if isinstance(value, Vector) else 1


class GeometryNode(dict):
    """JSON-compatible result with an out-of-band source location for kernel diagnostics."""

    def __init__(self, token, op, args, kwargs, children):
        super().__init__(op=op, args=args, kwargs=kwargs, children=children)
        self.line = token.line
        self.column = token.column


@dataclass
class Scope:
    parent: object = None
    values: dict = field(default_factory=dict)
    variables: dict = field(default_factory=dict)
    definitions: dict = field(default_factory=dict)
    resolving: set = field(default_factory=set)
    input_children: object = None

    def find(self, name, expressions=False):
        scope = self
        while scope is not None:
            if expressions and (name in scope.values or name in scope.variables):
                return scope
            if not expressions and name in scope.definitions:
                return scope
            scope = scope.parent
        return None


UNARY_MATH = {
    'abs': abs, 'sqrt': math.sqrt, 'floor': math.floor, 'ceil': math.ceil,
    'round': lambda x: math.floor(x+.5) if x >= 0 else math.ceil(x-.5),
    'sin': lambda x: math.sin(math.radians(x)), 'cos': lambda x: math.cos(math.radians(x)),
    'tan': lambda x: math.tan(math.radians(x)), 'asin': lambda x: math.degrees(math.asin(x)),
    'acos': lambda x: math.degrees(math.acos(x)), 'atan': lambda x: math.degrees(math.atan(x)),
    'sign': lambda x: (x>0)-(x<0), 'ln': math.log, 'log': math.log10, 'exp': math.exp,
}
SCALAR_OPS = {'+': operator.add, '-': operator.sub, '*': operator.mul, '/': operator.truediv,
              '%': math.fmod, '^': math.pow, '<': operator.lt, '<=': operator.le,
              '>': operator.gt, '>=': operator.ge}


class Evaluator:
    def __init__(self):
        self.work = 0
        self.geometry_count = 0

    def charge(self, token, amount=1):
        self.work += amount
        if self.work > MAX_WORK:
            raise ScadError(token, 'Script exceeds the evaluation budget')

    def visit(self, node, level):
        if level >= MAX_DEPTH:
            raise ScadError(node.token, 'Expression or module recursion is too deep')
        self.charge(node.token)

    def vector(self, items, token):
        result = Vector(items)
        if result.cost > MAX_VECTOR_SIZE or result.depth > MAX_DEPTH:
            raise ScadError(token, 'Vector exceeds the size or nesting limit')
        self.charge(token, result.cost)
        return result

    def geometry(self, token, name, args, kwargs, children):
        self.geometry_count += 1
        if self.geometry_count > MAX_GEOMETRY:
            raise ScadError(token, f'Script exceeds {MAX_GEOMETRY} geometry nodes')
        self.charge(token, 1+sum(map(value_cost, args))+sum(map(value_cost, kwargs.values())))
        return GeometryNode(token, name, args, kwargs, children)

    def group(self, nodes, token):
        if len(nodes) <= 1:
            return nodes
        return [self.geometry(token, 'union', [], {}, nodes)]

    def variable(self, name, scope, token, level):
        owner = scope.find(name, True)
        if owner is None:
            raise ScadError(token, f'Unknown variable {name!r}')
        if name not in owner.values:
            if name in owner.resolving:
                raise ScadError(token, f'Circular variable definition {name!r}')
            owner.resolving.add(name)
            try:
                owner.values[name] = self.expr(owner.variables[name], owner, level+1)
            finally:
                owner.resolving.remove(name)
        return owner.values[name]

    def arguments(self, arguments, scope, level):
        positional, named = [], {}
        for name, expr in arguments:
            value = self.expr(expr, scope, level+1)
            if name is None:
                positional.append(value)
            else:
                named[name] = value
        return positional, named

    def call_scope(self, node, owner, caller, args, kwargs, level, token):
        env = Scope(owner)
        params = node.parts[1]
        if len(args) > len(params):
            raise ScadError(token, f'Too many arguments for {node.parts[0]!r}')
        kwargs = dict(kwargs)
        for i, (key, default) in enumerate(params):
            if i < len(args):
                if key in kwargs:
                    raise ScadError(token, f'Duplicate argument {key!r}')
                env.values[key] = args[i]
            elif key in kwargs:
                env.values[key] = kwargs.pop(key)
            elif default is not None:
                env.values[key] = self.expr(default, env, level+1)
            else:
                raise ScadError(token, f'Missing argument {key!r}')
        if '$fn' not in env.values:
            if '$fn' in kwargs:
                env.values['$fn'] = kwargs.pop('$fn')
            elif caller.find('$fn', True):
                env.values['$fn'] = self.variable('$fn', caller, token, level)
        if kwargs:
            raise ScadError(token, f'Unknown argument {next(iter(kwargs))!r}')
        return env

    def iterations(self, bindings, scope, token, level):
        if level + len(bindings) >= MAX_DEPTH:
            raise ScadError(token, 'Loop is nested too deeply')

        def expand(i, env):
            if i == len(bindings):
                yield env
                return
            name, expr = bindings[i]
            values = self.expr(expr, env, level+i+1)
            if not isinstance(values, Vector):
                raise ScadError(token, 'for requires a range or vector')
            if len(values) > MAX_RANGE:
                raise ScadError(token, f'Loop sequence exceeds {MAX_RANGE} values')
            for value in values:
                self.charge(token)
                yield from expand(i+1, Scope(env, {name: value}))
        return expand(0, scope)

    def vector_arithmetic(self, op, a, b, t):
        av, bv = isinstance(a, Vector), isinstance(b, Vector)
        if av and bv and len(a) == len(b):
            if op in ('+', '-'):
                return self.vector((self.binary(op, x, y, t) for x, y in zip(a, b)), t)
            if op == '*':
                self.charge(t, len(a))
                return number(sum(number(x, t)*number(y, t) for x, y in zip(a, b)), t)
        if av and not bv and op in ('*', '/'):
            return self.vector((self.binary(op, x, b, t) for x in a), t)
        if bv and not av and op == '*':
            return self.vector((self.binary(op, a, y, t) for y in b), t)
        raise ScadError(t, 'Unsupported vector operation or mismatched vector sizes')

    def binary(self, op, a, b, t):
        if op in ('==', '!='):
            self.charge(t, value_cost(a)+value_cost(b))
            equal = (isinstance(a, bool) == isinstance(b, bool)) and a == b
            return equal if op == '==' else not equal
        if isinstance(a, Vector) or isinstance(b, Vector):
            return self.vector_arithmetic(op, a, b, t)
        try:
            value = SCALAR_OPS[op](number(a, t), number(b, t))
        except (ArithmeticError, ValueError) as exc:
            if isinstance(exc, ScadError):
                raise
            raise ScadError(t, f'Invalid arithmetic for {op!r}') from exc
        return value if type(value) is bool else number(value, t)

    def builtin(self, name, args, kwargs, t):
        if kwargs:
            raise ScadError(t, 'Built-in functions take positional arguments')
        if name == 'len' and len(args) == 1 and isinstance(args[0], Vector):
            return len(args[0])
        if name == 'norm' and len(args) == 1 and isinstance(args[0], Vector):
            self.charge(t, len(args[0]))
            return number(math.hypot(*(number(x, t) for x in args[0])), t)
        if name in ('min', 'max'):
            values = args[0] if len(args) == 1 and isinstance(args[0], Vector) else args
            if not values:
                raise ScadError(t, f'{name} requires at least one number')
            self.charge(t, len(values))
            return (min if name == 'min' else max)(number(x, t) for x in values)
        try:
            if name in UNARY_MATH and len(args) == 1:
                return number(UNARY_MATH[name](number(args[0], t)), t)
            if name == 'atan2' and len(args) == 2:
                return math.degrees(math.atan2(number(args[0], t), number(args[1], t)))
            if name == 'pow' and len(args) == 2:
                return self.binary('^', *args, t)
        except (ArithmeticError, ValueError) as exc:
            if isinstance(exc, ScadError):
                raise
            raise ScadError(t, f'Invalid input to {name}') from exc
        raise ScadError(t, f'Unknown function or invalid arguments for {name!r}')

    def expr(self, node, env, level):
        self.visit(node, level)
        t, kind, p = node.token, node.kind, node.parts

        def ev(expr):
            return self.expr(expr, env, level+1)

        if kind == 'literal':
            return p[0]
        if kind == 'var':
            return self.variable(p[0], env, t, level)
        if kind == 'vector':
            return self.vector((ev(x) for x in p[0]), t)
        if kind == 'range':
            start = number(ev(p[0]), t)
            mid = number(ev(p[1]), t)
            step, end = (1, mid) if p[2] is None else (mid, number(ev(p[2]), t))
            if step == 0:
                raise ScadError(t, 'Range step must be nonzero')
            span = (end-start)/step
            if span < 0:
                return self.vector([], t)
            if not math.isfinite(span) or span >= MAX_RANGE:
                raise ScadError(t, f'Range exceeds {MAX_RANGE} values')
            count = math.floor(span+1e-10)+1
            if count > MAX_RANGE:
                raise ScadError(t, f'Range exceeds {MAX_RANGE} values')
            return self.vector((number(start+i*step, t) for i in range(count)), t)
        if kind == 'comprehension':
            items = []
            for iteration in self.iterations(p[0], env, t, level):
                if len(items) >= MAX_RANGE:
                    raise ScadError(t, 'Vector comprehension is too large')
                items.append(self.expr(p[1], iteration, level+1))
            return self.vector(items, t)
        if kind == 'index':
            vector = ev(p[0])
            if not isinstance(vector, Vector):
                raise ScadError(t, 'Indexing requires a vector')
            return vector[index_number(ev(p[1]), len(vector), t)]
        if kind == 'ternary':
            return ev(p[1]) if ev(p[0]) else ev(p[2])
        if kind == 'unary':
            op, expr = p
            value = ev(expr)
            if op == '!':
                return not value
            return self.binary('*', -1 if op == '-' else 1, value, t)
        if kind == 'binary':
            op, left, right = p
            a = ev(left)
            if op == '&&':
                return bool(a) and bool(ev(right))
            if op == '||':
                return bool(a) or bool(ev(right))
            return self.binary(op, a, ev(right), t)
        if kind == 'function_call':
            name, arguments = p
            args, kwargs = self.arguments(arguments, env, level)
            owner = env.find(('function', name))
            if owner:
                definition = owner.definitions['function', name]
                scope = self.call_scope(definition, owner, env, args, kwargs, level, t)
                return self.expr(definition.parts[2], scope, level+1)
            return self.builtin(name, args, kwargs, t)
        raise ScadError(t, 'Invalid expression')

    def declarations(self, statements, env):
        geometry = []
        for node in statements:
            self.charge(node.token)
            if node.kind == 'assign':
                name, expr = node.parts
                env.variables[name] = expr
            elif node.kind in ('module', 'function'):
                env.definitions[node.kind, node.parts[0]] = node
            else:
                geometry.append(node)
        return geometry

    def sequence(self, statements, scope, level):
        nodes = []
        for statement in self.declarations(statements, scope):
            nodes.extend(self.statement(statement, scope, level+1))
        return nodes

    def children(self, args, kwargs, env, token, level):
        scope = env
        while scope and scope.input_children is None:
            scope = scope.parent
        if not scope:
            raise ScadError(token, 'children() can only be used inside a module')
        statements, owner = scope.input_children
        if kwargs or len(args) > 1:
            raise ScadError(token, 'Use children() or children(index)')
        if args:
            statements = [statements[index_number(args[0], len(statements), token)]]
        nodes = []
        for statement in statements:
            nodes.extend(self.statement(statement, owner, level+1))
        return self.group(nodes, token)

    def statement(self, node, env, level):
        self.visit(node, level)
        t, kind, p = node.token, node.kind, node.parts
        if kind == 'block':
            return self.group(self.sequence(p[0], Scope(env), level), t)
        if kind == 'for':
            nodes = []
            for child_scope in self.iterations(p[0], env, t, level):
                nodes.extend(self.statement(p[1], child_scope, level+1))
            return self.group(nodes, t)
        if kind == 'if':
            choice = p[1] if self.expr(p[0], env, level+1) else p[2]
            return self.statement(choice, Scope(env), level+1) if choice else []
        if kind in ('assign', 'module', 'function'):
            self.declarations([node], env)
            return []
        name, arguments, children = p
        args, kwargs = self.arguments(arguments, env, level)
        owner = env.find(('module', name))
        if owner:
            definition = owner.definitions['module', name]
            child_env = self.call_scope(definition, owner, env, args, kwargs, level, t)
            caller = Scope(env)
            child_statements = self.declarations(children, caller)
            child_env.values['$children'] = len(child_statements)
            child_env.input_children = child_statements, caller
            return self.group(self.statement(definition.parts[2], child_env, level+1), t)
        if name == 'children':
            if children:
                raise ScadError(t, 'children() cannot take child geometry')
            return self.children(args, kwargs, env, t, level)
        if name not in GEOMETRY:
            raise ScadError(t, f'Unknown or unsupported geometry command {name!r}')
        child_scope = Scope(env)
        if '$fn' in kwargs:
            child_scope.values['$fn'] = kwargs['$fn']
            if name not in FACET_OPERATIONS:
                kwargs.pop('$fn')
        elif name in FACET_OPERATIONS and env.find('$fn', True):
            kwargs['$fn'] = self.variable('$fn', env, t, level)
        return [self.geometry(t, name, args, kwargs, self.sequence(children, child_scope, level))]


def evaluate(source, parameters=None):
    """Parse and expand a source document into bounded geometry nodes.

    Numeric parameter overrides are visible at top level (including overrides of
    assigned defaults). Ordinary variables and definitions have lexical scope.
    """
    parser = Parser(source)
    model = parser.program()
    start = Token('', 1, 1)
    if parameters is None:
        parameters = {}
    if not isinstance(parameters, dict) or len(parameters) > 128:
        raise ScadError(start, 'Supply at most 128 named numeric parameters')
    base = Scope(values={'true': True, 'false': False, 'PI': math.pi})
    env = Scope(base)
    for name, value in parameters.items():
        if not isinstance(name, str) or not IDENTIFIER.fullmatch(name) or name in RESERVED:
            raise ScadError(start, 'Invalid numeric parameter name')
        env.values[name] = number(value, start)
    return Evaluator().sequence(model, env, 0)
