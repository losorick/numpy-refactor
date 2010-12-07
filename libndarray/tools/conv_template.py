"""
takes templated file .xxx.src and produces .xxx file  where .xxx is
.i or .c or .h, using the following template rules

/**begin repeat  -- on a line by itself marks the start of a repeated code
                    segment
/**end repeat**/ -- on a line by itself marks it's end

After the /**begin repeat and before the */, all the named templates are placed
these should all have the same number of replacements

Repeat blocks can be nested, with each nested block labeled with its depth,
i.e.
/**begin repeat1
 *....
 */
/**end repeat1**/

When using nested loops, you can optionally exlude particular
combinations of the variables using (inside the comment portion of the inner
loop):

 :exclude: var1=value1, var2=value2, ...

This will exlude the pattern where var1 is value1 and var2 is value2 when
the result is being generated.


In the main body each replace will use one entry from the list of named
replacements

 Note that all #..# forms in a block must have the same number of
   comma-separated entries.
"""

import os
import re
import glob

# names for replacement that are already global.
global_names = {}

# header placed at the front of head processed file
header = """
/*
 **************************************************************************
 **     This file was autogenerated from a template  DO NOT EDIT!!!!     **
 **     Changes should be made to the original source (.src) file        **
 **************************************************************************
 */

"""
# Parse string for repeat loops
def parse_structure(astr, level):
    """
    The returned line number is from the beginning of the string, starting
    at zero. Returns an empty list if no loops found.

    """
    if level == 0:
        loopbeg = "/**begin repeat"
        loopend = "/**end repeat**/"
    else:
        loopbeg = "/**begin repeat%d" % level
        loopend = "/**end repeat%d**/" % level

    ind = 0
    line = 0
    spanlist = []
    while 1:
        start = astr.find(loopbeg, ind)
        if start == -1:
            break
        start2 = astr.find("*/", start)
        start2 = astr.find("\n", start2)
        fini1 = astr.find(loopend, start2)
        fini2 = astr.find("\n", fini1)
        line += astr.count("\n", ind, start2 + 1)
        spanlist.append((start, start2 + 1, fini1, fini2 + 1, line))
        line += astr.count("\n", start2 + 1, fini2)
        ind = fini2
    spanlist.sort()
    return spanlist


def paren_repl(obj):
    torep = obj.group(1)
    numrep = obj.group(2)
    return ','.join([torep] * int(numrep))


parenrep = re.compile(r"[(]([^)]*)[)]\*(\d+)")
plainrep = re.compile(r"([^*]+)\*(\d+)")
def parse_values(astr):
    # replaces all occurrences of '(a,b,c)*4' in astr
    # with 'a,b,c,a,b,c,a,b,c,a,b,c'. Empty braces generate
    # empty values, i.e., ()*4 yields ',,,'. The result is
    # split at ',' and a list of values returned.
    astr = parenrep.sub(paren_repl, astr)
    # replaces occurences of xxx*3 with xxx, xxx, xxx
    astr = ','.join([plainrep.sub(paren_repl,x.strip())
                     for x in astr.split(',')])
    return astr.split(',')


stripast = re.compile(r"\n\s*\*?")
named_re = re.compile(r"#\s*(\w*)\s*=([^#]*)#")
exclude_vars_re = re.compile(r"(\w*)=(\w*)")
exclude_re = re.compile(":exclude:")
def parse_loop_header(loophead):
    """Find all named replacements in the header

    Returns a list of dictionaries, one for each loop iteration,
    where each key is a name to be substituted and the corresponding
    value is the replacement string.

    Also return a list of exclusions.  The exclusions are dictionaries
     of key value pairs. There can be more than one exclusion.
     [{'var1':'value1', 'var2', 'value2'[,...]}, ...]
    """
    # Strip out '\n' and leading '*', if any, in continuation lines.
    # This should not effect code previous to this change as
    # continuation lines were not allowed.
    loophead = stripast.sub("", loophead)
    # parse out the names and lists of values
    names = []
    reps = named_re.findall(loophead)
    nsub = None
    for rep in reps:
        name = rep[0]
        vals = parse_values(rep[1])
        size = len(vals)
        if nsub is None:
            nsub = size
        elif nsub != size:
            msg = "Mismatch in number of values:\n%s = %s" % (name, vals)
            raise ValueError(msg)
        names.append((name,vals))

    # Find any exclude variables
    excludes = []

    for obj in exclude_re.finditer(loophead):
        span = obj.span()
        # find next newline
        endline = loophead.find('\n', span[1])
        substr = loophead[span[1]:endline]
        ex_names = exclude_vars_re.findall(substr)
        excludes.append(dict(ex_names))

    # generate list of dictionaries, one for each template iteration
    dlist = []
    if nsub is None:
        raise ValueError("No substitution variables found")
    for i in range(nsub):
        tmp = {}
        for name,vals in names:
            tmp[name] = vals[i]
        dlist.append(tmp)
    return dlist


replace_re = re.compile(r"@([\w]+)@")
def parse_string(astr, env, level, line):
    # local function for string replacement, uses env
    def replace(match):
        name = match.group(1)
        try:
            val = env[name]
        except KeyError:
            msg = 'line %d: no definition of key "%s"' % (line, name)
            raise ValueError(msg)
        return val

    code = []
    struct = parse_structure(astr, level)
    if struct:
        # recurse over inner loops
        oldend = 0
        newlevel = level + 1
        for sub in struct:
            pref = astr[oldend:sub[0]]
            head = astr[sub[0]:sub[1]]
            text = astr[sub[1]:sub[2]]
            oldend = sub[3]
            newline = line + sub[4]
            code.append(replace_re.sub(replace, pref))
            for newenv in parse_loop_header(head):
                newenv.update(env)
                newcode = parse_string(text, newenv, newlevel, newline)
                code.extend(newcode)
        suff = astr[oldend:]
        code.append(replace_re.sub(replace, suff))
    else:
        # replace keys
        code.append(replace_re.sub(replace, astr))
    code.append('\n')
    return ''.join(code)


def process_str(astr):
    code = [header]
    code.extend(parse_string(astr, global_names, 0, 1))
    return ''.join(code)


def process_file(src):
    assert src.endswith('.src')
    dst = src[:-4]

    if not os.path.exists(dst) or os.path.getmtime(dst) <= os.path.getmtime(src):
        print "\tProcessing %s into %s" % (src, dst)
        data = open(src).read()
        data = process_str(data)

        fo = open(dst, 'w')
        fo.write(data)
        fo.close()


if __name__ == "__main__":
    import sys

    print "Processing code templates:"
    for arg in sys.argv[1:]:
        for file in glob.glob(arg):
            process_file(file)
