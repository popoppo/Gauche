/*
 * regexp.c - regular expression
 *
 *  Copyright(C) 2000-2001 by Shiro Kawai (shiro@acm.org)
 *
 *  Permission to use, copy, modify, distribute this software and
 *  accompanying documentation for any purpose is hereby granted,
 *  provided that existing copyright notices are retained in all
 *  copies and that this notice is included verbatim in all
 *  distributions.
 *  This software is provided as is, without express or implied
 *  warranty.  In no circumstances the author(s) shall be liable
 *  for any damages arising out of the use of this software.
 *
 *  $Id: regexp.c,v 1.6 2001-04-15 12:37:02 shiro Exp $
 */

#include <setjmp.h>
#include "gauche.h"

SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_RegexpClass, NULL);
SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_RegMatchClass, NULL);

#ifndef CHAR_MAX
#define CHAR_MAX 256
#endif

/* THIS CODE IS EXPERIMENTAL, AND NOT WORKING YET. */

/* I don't like to reinvent wheels, so I looked for a regexp implementation
 * that can handle multibyte encodings and not bound to Unicode.
 * Without assuming Unicode it'll be difficult to define character classes
 * correctly, but there are domains that you don't want to do native
 * charset <-> UTF-8 each time for regexp match, trading correctness of
 * character classes.
 *
 * The most recent version of famous Henry Spencer's regex is found in Tcl
 * 8.3, that supports wide characters (the state machine seems to work
 * with UCS-4, but the internal tables seem to be set up for UCS-2 only).
 * Tcl does UTF-8 <-> UCS-2 conversion in order to do regexp match.
 *
 * Lots of variants of Spencer's old regex code is floating around, such
 * as http://arglist.com/regex/ and the one in BSD.   They don't support
 * multibyte strings, as far as I know.
 *
 * Another popular package is PCRE.  PCRE 3.4 has UTF-8 support, but only
 * experimentally.
 *
 * None seems to satisfy my criteria.
 *
 * So I reluctantly started to write my own.  I don't think I can beat
 * those guys, and am willing to grab someone's code anytime if it's suitable
 * for my purpose and under a license like BSD one.  
 */

/*
 * The idea here is to match string without converting mb <-> char as
 * much as possible.  The conversion is inevitable anytime we have to
 * match charsets, but there are number of cases we can avoid it.
 * The engine itself will be implemented in NFA-based scheme.
 */

/* Instructions */
enum {
    RE_MATCH1,                  /* followed by 1 byte to match */
    RE_MATCH,                   /* followed by length, and bytes to match */
    RE_ANY,                     /* match any char (maybe but newline) */
    RE_TRY,                     /* followed by offset.  try matching
                                   the following sequence, and if fails,
                                   jump to offset. */
    RE_SET,                     /* followed by a number of charset */
    RE_JUMP,                    /* followed by offset.  jump to that
                                   bytecode. */
    RE_FAIL,                    /* fail */
    RE_SUCCESS,                 /* success */
    RE_BEGIN,                   /* followed by a group number.  start the
                                   group. */
    RE_END                      /* followed by a group number.  end the
                                   group. */
};

/* symbols used internally */
ScmObj sym_alt;                 /* alt */
ScmObj sym_rep;                 /* rep */
ScmObj sym_any;                 /* any */

static ScmRegexp *make_regexp(void)
{
    ScmRegexp *rx = SCM_NEW(ScmRegexp);
    SCM_SET_CLASS(rx, SCM_CLASS_REGEXP);
    rx->code = NULL;
    rx->numCodes = 0;
    rx->numGroups = 0;
    rx->numSets = 0;
    rx->sets = NULL;
    rx->mustMatch = NULL;
    rx->mustMatchLen = 0;
    return rx;
}

/*=======================================================================
 * Compiler
 */

/* Two-pass compiler.
 *
 *  pass 1: parse pattern, generating parse tree and also counting sizes
 *          of necessary storage.
 *  pass 2: byte code generation.
 */

/* compiler state information */
struct comp_ctx {
    ScmString *pattern;         /* original pattern */
    const char *rxstr;          /* [pass1] current point being parsed */
    int rxlen;                  /* [pass1] current length */
    ScmObj sets;                /* [pass1] list of charsets */
    char *code;                 /* [pass2] code being built */
    int codep;                  /* [pass2] front of code generation */
    int codemax;                /* [pass2] max codep */
};

static ScmObj re_compile_charset(ScmRegexp *rx, struct comp_ctx *ctx);

static inline ScmChar fetch_pattern(struct comp_ctx *ctx)
{
    ScmChar ch;
    SCM_STR_GETC(ctx->rxstr, ch);
    ctx->rxstr += SCM_CHAR_NBYTES(ch);
    ctx->rxlen--;
    return ch;
}

/* Util function in pass1.  look back the parser tree to find out
   the last branch of the parse tree. */
static ScmObj last_item(struct comp_ctx *ctx, ScmObj head, ScmObj tail,
                        ScmObj gstack, ScmChar ch)
{
    ScmObj last = SCM_CAR(tail);
    if (SCM_INTP(last)) {
        int gnum = SCM_INT_VALUE(last);
        if (gnum < 0) {
            /* just after close parenthesis.
               find the begining of the group. */
            ScmObj cp, gstart = SCM_MAKE_INT(-gnum);
            SCM_FOR_EACH(cp, head) {
                if (SCM_CAR(cp) == gstart) return cp;
            }
            Scm_Error("something broken internally.");
        } else {
            /* just after open parenthesis or beginning. */
            if (ch == '|') {
                /* notimpl */
                Scm_Error("not implemented");
            } else {
                Scm_Error("bad regexp pattern: %S", ctx->pattern);
            }
        }
    } else {
        if (ch == '|') {
            return SCM_CDAR(gstack);
        } else {
            return tail;
        }
    }
}

/* Util function in pass1.  Fold the last branch into alternative subtree
   if necessary. */
static ScmObj fold_alternatives(ScmObj head, ScmObj tail, ScmObj grpnum)
{
    ScmObj cp;
    SCM_FOR_EACH(cp, head) {
        if (SCM_CAR(cp) == grpnum) {
            if (SCM_PAIRP(SCM_CDR(cp)) && SCM_PAIRP(SCM_CADR(cp))) {
                ScmObj clause = SCM_CADR(cp);
                if (SCM_CAR(clause) == sym_alt) {
                    SCM_SET_CDR(Scm_LastPair(clause),
                                SCM_LIST1(SCM_CDDR(cp)));
                    SCM_SET_CDR(SCM_CDR(cp), SCM_NIL);
                    return SCM_CDR(cp);
                }
            }
            break;
        }
    }
    return tail;
}

/*----------------------------------------------------------------
 * pass1 - parser
 */
ScmObj re_compile_pass1(ScmRegexp *rx, struct comp_ctx *ctx)
{
    ScmObj head = SCM_NIL, tail = SCM_NIL, elt, cell, sym;
    ScmObj grpstack;            /* group stack. */
    ScmChar ch;
    int grpcount = 0;
    int insncount = 0;

    /* default group == entire match*/
    SCM_APPEND1(head, tail, SCM_MAKE_INT(0));
    grpstack = Scm_Cons(tail, SCM_NIL);
    
    while (ctx->rxlen > 0) {
        ch = fetch_pattern(ctx);

        switch (ch) {
        case '(':
            grpcount++;
            insncount++;
            SCM_APPEND1(head, tail, SCM_MAKE_INT(grpcount));
            grpstack = Scm_Cons(tail, grpstack);
            continue;
        case ')':
            if (SCM_NULLP(SCM_CDR(grpstack))) {
                Scm_Error("extra close parenthesis in regexp: %S",
                          ctx->pattern);
            } else {
                ScmObj gnum = SCM_CAAR(grpstack);
                int g = SCM_INT_VALUE(gnum);
                tail = fold_alternatives(head, tail, gnum);
                SCM_APPEND1(head, tail, SCM_MAKE_INT(-g));
                grpstack = SCM_CDR(grpstack);
                insncount++;
            }
            continue;
        case '|':
            elt = last_item(ctx, head, tail, grpstack, ch);
            if (SCM_PAIRP(SCM_CAR(elt))
                && SCM_CAAR(elt) == sym_alt) {
                tail = fold_alternatives(head, tail, SCM_CAAR(grpstack));
            } else {
                cell = Scm_Cons(SCM_CAR(elt), SCM_CDR(elt));
                SCM_SET_CAR(elt, SCM_LIST2(sym_alt, cell));
                SCM_SET_CDR(elt, SCM_NIL);
                tail = elt;
            }
            insncount += 4;
            continue;
        case '+':  /* x+ === xx* */
            elt = Scm_CopyList(last_item(ctx, head, tail, grpstack, ch));
            SCM_APPEND1(head, tail, Scm_Cons(sym_rep, elt));
            insncount++;
            continue;
        case '?':  /* x? === (x|) */
            elt = last_item(ctx, head, tail, grpstack, ch);
            cell = Scm_Cons(SCM_CAR(elt), SCM_CDR(elt));
            SCM_SET_CAR(elt, SCM_LIST3(sym_alt, cell, SCM_NIL));
            SCM_SET_CDR(elt, SCM_NIL);
            tail = elt;
            insncount += 5;
            continue;
        case '*':
            elt = last_item(ctx, head, tail, grpstack, ch);
            cell = Scm_Cons(SCM_CAR(elt), SCM_CDR(elt));
            SCM_SET_CAR(elt, Scm_Cons(sym_rep, cell));
            SCM_SET_CDR(elt, SCM_NIL);
            tail = elt;
            insncount += 3;
            continue;
        case '.':
            SCM_APPEND1(head, tail, sym_any);
            insncount++;
            continue;
        case '[':
            SCM_APPEND1(head, tail, re_compile_charset(rx, ctx));
            insncount++;
            continue;
        default:
            insncount += SCM_CHAR_NBYTES(ch);
            SCM_APPEND1(head, tail, SCM_MAKE_CHAR(ch));
        }
    }

    tail = fold_alternatives(head, tail, SCM_MAKE_INT(0));
    SCM_ASSERT(SCM_PAIRP(grpstack));
    if (!SCM_NULLP(SCM_CDR(grpstack)))
        Scm_Error("extra open parenthesis in regexp: %S", ctx->pattern);

    rx->numGroups = grpcount+1;
    rx->numCodes = insncount+1;
    return head;
}

/* character range */
/* TODO:  [:class:] and other posix weird stuff. */
static ScmObj re_compile_charset(ScmRegexp *rx, struct comp_ctx *ctx)
{
    int begin = TRUE, complement = FALSE;
    int lastchar = -1, inrange = FALSE;
    ScmCharSet *set = SCM_CHARSET(Scm_MakeEmptyCharSet());
    ScmChar ch;

    for (;;) {
        if (ctx->rxlen <= 0) Scm_Error("Unclosed bracket: %S", ctx->pattern);
        ch = fetch_pattern(ctx);

        if (begin) {
            if (ch == '^') complement = TRUE;
            else {
                Scm_CharSetAddRange(set, ch, ch);
                lastchar = ch;
            }
            begin = FALSE;
            continue;
        }
        switch (ch) {
        case '-':
            if (lastchar < 0) {
                Scm_Error("bad character range spec: %S", ctx->pattern);
            }
            inrange = TRUE;
            continue;
        case ']':
            if (inrange) {
                SCM_ASSERT(lastchar >= 0);
                Scm_CharSetAddRange(set, lastchar, lastchar);
                Scm_CharSetAddRange(set, '-', '-');
            }
            break;
        default:
            if (inrange) {
                SCM_ASSERT(lastchar >= 0);
                Scm_CharSetAddRange(set, lastchar, ch);
                lastchar = -1;
                inrange = FALSE;
            } else {
                Scm_CharSetAddRange(set, ch, ch);
                lastchar = ch;
            }
            continue;
        }
        break;
    }
    if (complement) Scm_CharSetComplement(set);
    ctx->sets = Scm_Cons(SCM_OBJ(set), ctx->sets);
    return SCM_OBJ(set);
}

/* An interlude between pass1 and pass2.  From the information of
 * parser context, build a charset vector.
 */
static void re_compile_setup_charsets(ScmRegexp *rx, struct comp_ctx *ctx)
{
    ScmObj cp;
    rx->numSets = 0;
    SCM_FOR_EACH(cp, Scm_Reverse(ctx->sets)) {
        rx->sets[rx->numSets++] = SCM_CHARSET(SCM_CAR(cp));
    }
}

/* Util function for pass2, to get an index of the charset vector
 * for the given charset.
 */
static int re_compile_charset_index(ScmRegexp *rx, ScmObj cs)
{
    int i;
    for (i=0; i<rx->numSets; i++)
        if (cs == SCM_OBJ(rx->sets[i])) return i;
    Scm_Panic("re_compile_charset_index: can't be here");
    return 0;                   /* dummy */
}

static inline void re_compile_emit(struct comp_ctx *ctx, char code)
{
    SCM_ASSERT(ctx->codep < ctx->codemax);
    ctx->code[ctx->codep++] = code;
}

/*-------------------------------------------------------------
 * pass 2 - code generation
 */
void re_compile_pass2(ScmObj compiled, ScmRegexp *rx, struct comp_ctx *ctx)
{
    ScmObj cp, item;
    ScmChar ch;
    char chbuf[SCM_CHAR_MAX_BYTES];

    SCM_FOR_EACH(cp, compiled) {
        if (SCM_NULLP(cp)) break;
        item = SCM_CAR(cp);

        /* literal characters */
        if (SCM_CHARP(item)) {
            /* find out the longest run of bytes */
            int nrun = 0, ocodep = ctx->codep, nb, i;
            re_compile_emit(ctx, RE_MATCH);
            re_compile_emit(ctx, 0); /* patched later */
            do {
                ch = SCM_CHAR_VALUE(item);
                nb = SCM_CHAR_NBYTES(ch);
                SCM_STR_PUTC(chbuf, SCM_CHAR_VALUE(item));
                for (i=0; i<nb; i++) re_compile_emit(ctx, chbuf[i]);
                nrun += nb;
                if (SCM_NULLP(SCM_CDR(cp))) break;
                cp = SCM_CDR(cp);
                item = SCM_CAR(cp);
            } while (SCM_CHARP(item) && nrun < CHAR_MAX);
            if (nrun == 1) {
                ctx->code[ocodep] = RE_MATCH1;
                ctx->code[ocodep+1] = ctx->code[ocodep+2];
                ctx->codep = ocodep+2;
            } else {
                ctx->code[ocodep+1] = (char)nrun;
            }
            if (!SCM_CHARP(item)) cp = Scm_Cons(item, cp); /* pushback */
            continue;
        }

        /* group start/end */
        if (SCM_INTP(item)) {
            int grpnum = SCM_INT_VALUE(item);
            if (grpnum < 0) {
                re_compile_emit(ctx, RE_END);
                re_compile_emit(ctx, -grpnum);
            } else {
                re_compile_emit(ctx, RE_BEGIN);
                re_compile_emit(ctx, grpnum);
            }
            continue;
        }

        /* charset */
        if (SCM_CHARSETP(item)) {
            re_compile_emit(ctx, RE_SET);
            re_compile_emit(ctx, re_compile_charset_index(rx, item));
            continue;
        }

        /* special stuff */
        if (SCM_SYMBOLP(item)) {
            if (item == sym_any) {
                re_compile_emit(ctx, RE_ANY);
                continue;
            }
            /* fallback to error */
        }

        if (SCM_PAIRP(item)) {
            ScmObj car = SCM_CAR(item);
            int ocodep;

            if (car == sym_rep) {
                ocodep = ctx->codep;
                re_compile_emit(ctx, RE_TRY);
                re_compile_emit(ctx, 0); /* will be patched */
                re_compile_pass2(SCM_CDR(item), rx, ctx);
                re_compile_emit(ctx, RE_JUMP);
                re_compile_emit(ctx, ocodep);
                ctx->code[ocodep+1] = ctx->codep;
                continue;
            }
            if (car == sym_alt) {
                ScmObj clause;
                ScmObj jumps = SCM_NIL;
                int patchp;

                for (clause = SCM_CDR(item);
                     SCM_PAIRP(SCM_CDR(clause));
                     clause = SCM_CDR(clause)) {
                    re_compile_emit(ctx, RE_TRY);
                    patchp = ctx->codep;
                    re_compile_emit(ctx, 0); /* will be patched */
                    re_compile_pass2(SCM_CAR(clause), rx, ctx);
                    re_compile_emit(ctx, RE_JUMP);
                    jumps = Scm_Cons(SCM_MAKE_INT(ctx->codep), jumps);
                    re_compile_emit(ctx, 0); /* will be patched */
                    ctx->code[patchp] = ctx->codep;
                }
                re_compile_pass2(SCM_CAR(clause), rx, ctx);
                SCM_FOR_EACH(jumps, jumps) {
                    ctx->code[SCM_INT_VALUE(SCM_CAR(jumps))] = ctx->codep;
                }
                continue;
            }
            /* fallback to error */
        }

        Scm_Error("internal error while rexexp compilation: item %S\n", item);
    }
}

/* For debug */
void Scm_RegDump(ScmRegexp *rx)
{
    int end = rx->numCodes, codep;

    printf("Regexp %p:\n", rx);
    printf("  must = ");
    if (rx->mustMatchLen > 0) {
        int i;
        printf("'");
        for (i=0; i<rx->mustMatchLen; i++) printf("%c", rx->mustMatch[i]);
        printf("'\n");
    } else {
        printf("(none)\n");
    }

    for (codep = 0; codep < end; codep++) {
        switch (rx->code[codep]) {
        case RE_MATCH1:
            codep++;
            printf("%4d  MATCH1  0x%02x  '%c'\n",
                   codep-1, rx->code[codep], rx->code[codep]);
            continue;
        case RE_MATCH:
            codep++;
            {
                u_int numchars = (u_int)rx->code[codep];
                int i;
                printf("%4d  MATCH(%3d) '", codep-1, numchars);
                for (i=0; i< numchars; i++)
                    printf("%c", rx->code[++codep]);
                printf("'\n");
            }
            continue;
        case RE_ANY:
            printf("%4d  ANY\n", codep);
            continue;
        case RE_TRY:
            codep++;
            printf("%4d  TRY  %d\n", codep-1, rx->code[codep]);
            continue;
        case RE_SET:
            codep++;
            printf("%4d  SET  %d\n", codep-1, rx->code[codep]);
            continue;
        case RE_JUMP:
            codep++;
            printf("%4d  JUMP %d\n", codep-1, rx->code[codep]);
            continue;
        case RE_FAIL:
            printf("%4d  FAIL\n", codep);
            continue;
        case RE_SUCCESS:
            printf("%4d  SUCCESS\n", codep);
            continue;
        case RE_BEGIN:
            codep++;
            printf("%4d  BEGIN %d\n", codep-1, rx->code[codep]);
            continue;
        case RE_END:
            codep++;
            printf("%4d  END %d\n", codep-1, rx->code[codep]);
            continue;
        default:
            Scm_Error("regexp screwed up\n");
        }
    }
}

/*--------------------------------------------------------------
 * Compiler entry point
 */
ScmObj Scm_RegComp(ScmString *pattern)
{
    ScmRegexp *rx = make_regexp();
    ScmObj compiled;
    struct comp_ctx cctx;
    cctx.pattern = pattern;
    cctx.rxstr = SCM_STRING_START(pattern);
    cctx.rxlen = SCM_STRING_LENGTH(pattern);
    cctx.sets = SCM_NIL;
    cctx.codep = 0;

    compiled = re_compile_pass1(rx, &cctx);
    Scm_Printf(SCM_CUROUT, "~~ %S %d\n", compiled, rx->numCodes);
    re_compile_setup_charsets(rx, &cctx);

    cctx.code = SCM_NEW_ATOMIC2(char *, rx->numCodes*2+1);
    cctx.codemax = rx->numCodes*2+1;
    re_compile_pass2(compiled, rx, &cctx);
    re_compile_emit(&cctx, RE_SUCCESS);
    rx->code = cctx.code;
    rx->numCodes = cctx.codep;

    return SCM_OBJ(rx);
}

/*=======================================================================
 * Matcher
 */

/* For now, I use C-stack directly to keep information for backtrack,
 * i.e. anytime I should try something I recursively call re_exec_rec().
 * It may run out the stack space if regexp requires deep recursion.
 * Possible optimization is to treat trivial cases (like '\S*') specially
 * to avoid recursion.
 *
 * Re_exec_rec doesn't return as long as match succeeds.  At the end of
 * code, it longjmp's to the start of matcher.
 *
 * My preliminary test showed that using C-stack & longjmp is faster than
 * allocating and maintaining the stack by myself.   Further test is required
 * for practical case, though.
 */

struct match_list {
    struct match_list *next;
    int grpnum;
    const char *ptr;
};

struct match_ctx {
    ScmRegexp *rx;
    const char *codehead;
    const char *stop;           /* end of input */
    const char *last;
    struct match_list *matches;
    jmp_buf cont;
};

static inline struct match_list *push_match(struct match_list *mlist,
                                            int grpnum, const char *ptr)
{
    struct match_list *elt = SCM_NEW(struct match_list);
    elt->next = mlist;
    elt->grpnum = grpnum;
    elt->ptr = ptr;
    return elt;
}

void re_exec_rec(const char *code,
                 const char *input,
                 struct match_ctx *ctx,                 
                 struct match_list *mlist)
{
    int param;
    ScmChar ch;
    ScmCharSet *cset;

    for (;;) {
        switch(*code++) {
        case RE_MATCH:
            param = (unsigned char)*code++;
            if (ctx->stop - input < param) return;
            while (param-- > 0) {
                if (*code++ != *input++) return;
            }
            continue;
        case RE_MATCH1:
            if (ctx->stop == input) return;
            if (*code++ != *input++) return;
            continue;
        case RE_ANY:
            param = SCM_CHAR_NFOLLOWS(*input);
            input += param+1;
            continue;
        case RE_TRY:
            param = (unsigned char)*code++;
            re_exec_rec(code, input, ctx, mlist);
            code = ctx->codehead + param;
            continue;
        case RE_JUMP:
            param = (unsigned char)*code++;
            code = ctx->codehead + param;
            continue;
        case RE_SET:
            param = (unsigned char)*code++;
            SCM_STR_GETC(input, ch);
            cset = ctx->rx->sets[param];
            if (!Scm_CharSetContains(cset, ch)) return;
            input += SCM_CHAR_NBYTES(ch);
            continue;
        case RE_BEGIN:
            param = (unsigned char)*code++;
            mlist = push_match(mlist, param, input);
            continue;
        case RE_END:
            param = (unsigned char)*code++;
            mlist = push_match(mlist, -param, input);
            continue;
        case RE_SUCCESS:
            ctx->last = input;
            ctx->matches = mlist;
            longjmp(ctx->cont, 1);
            /*NOTREACHED*/
        case RE_FAIL:
            return;
        }
    }
}

static ScmObj make_match(ScmRegexp *rx, ScmString *orig,
                         struct match_ctx *ctx)
{
    int i;
    struct match_list *ml;
    ScmRegMatch *rm = SCM_NEW(ScmRegMatch);
    SCM_SET_CLASS(rm, SCM_CLASS_REGMATCH);
    rm->numMatches = rx->numGroups;
    rm->matches = SCM_NEW2(struct ScmRegMatchSub*,
                           sizeof(struct ScmRegMatchSub)*rx->numGroups);
    rm->input = orig;
    for (i=0; i<rx->numGroups; i++) {
        rm->matches[i].index = -1;
        rm->matches[i].len = -1;
        rm->matches[i].start = NULL;
        rm->matches[i].end = NULL;
    }

    rm->matches[0].end = ctx->last;
    /* scan through match result */
    for (ml = ctx->matches; ml; ml = ml->next) {
        if (ml->grpnum >= 0) {
            rm->matches[ml->grpnum].start = ml->ptr;
        } else {
            rm->matches[-ml->grpnum].end = ml->ptr;
        }
    }

    /* sanity check (not necessary, but for now...) */
    for (i=0; i<rx->numGroups; i++) {
        if (!rm->matches[i].start || !rm->matches[i].end) {
            Scm_Panic("discrepancy in regexp match!");
        }
    }
    return SCM_OBJ(rm);
}

static ScmObj re_exec(ScmRegexp *rx, ScmString *orig,
                      const char *start, const char *end)
{
    struct match_ctx ctx;
    ctx.rx = rx;
    ctx.codehead = rx->code;
    ctx.stop = end;
    ctx.matches = NULL;

    if (setjmp(ctx.cont) == 0) {
        re_exec_rec(ctx.codehead, start, &ctx, NULL);
        return SCM_FALSE;
    } else {
        return make_match(rx, orig, &ctx);
    }
}

/*----------------------------------------------------------------------
 * entry point
 */
ScmObj Scm_RegExec(ScmRegexp *rx, ScmString *str)
{
    const char *start = SCM_STRING_START(str);
    const char *end = start + SCM_STRING_SIZE(str);
    /* TODO: prescreening */

    for (; start < end - rx->mustMatchLen; start++) {
        ScmObj r = re_exec(rx, str, start, end);
        if (!SCM_FALSEP(r)) return r;
    }
    return SCM_FALSE;
}

/*=======================================================================
 * Retrieving matches
 */

/* for debug */
void Scm_RegMatchDump(ScmRegMatch *rm)
{
    const char *cp;
    int i;
    
    Scm_Printf(SCM_CUROUT, "RegMatch %p\n", rm);
    Scm_Printf(SCM_CUROUT, "  numMatches = %d\n", rm->numMatches);
    Scm_Printf(SCM_CUROUT, "  input = %S\n", rm->input);
    for (i=0; i<rm->numMatches; i++) {
        Scm_Printf(SCM_CUROUT, "[%3d-%3d]  %S\n",
                   rm->matches[i].start - SCM_STRING_START(rm->input),
                   rm->matches[i].end - SCM_STRING_START(rm->input),
                   Scm_MakeStringConst(rm->matches[i].start,
                                       rm->matches[i].end - rm->matches[i].start,
                                       -1));
    }
}

/*=======================================================================
 * Initializing stuff
 */

void Scm__InitRegexp(void)
{
    Scm_InitBuiltinClass(SCM_CLASS_REGEXP, "<regexp>", Scm_GaucheModule());
    Scm_InitBuiltinClass(SCM_CLASS_REGMATCH, "<regmatch>", Scm_GaucheModule());

    sym_alt = SCM_INTERN("alt");
    sym_rep = SCM_INTERN("rep");
    sym_any = SCM_INTERN("any");
}
