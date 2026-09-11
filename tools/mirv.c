#include <libgrad/internal/base.h>

#define MRV_TOKEN_STREAM_BLOCK_CAPACITY 1024
#define MRV_MAX_ERR_LEN 1024


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// error reporting & the very important span
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
MRV_Span {
    size_t offset;
    size_t len;
} MRV_Span;

typedef struct
MRV_Error {
    bool       is_err;
    LG_Writer *writer;
    MRV_Span   span;
} MRV_Error;

void
mrv_report_error(MRV_Error *err, MRV_Span span, lg_str8 fmt, ...) {
    if (err->is_err) {
        return;
    }

    err->is_err = 1;
    err->span = span;

    lg_printf(err->writer, lg_str8_lit("at offsets %{i64}-%{i64}:\n"), span.offset, span.offset + span.len);

    va_list ap;
    va_start(ap, fmt);
    LG_StatusKind vprintf_status = lg_vprintf(err->writer, fmt, ap);
    (void)vprintf_status;
    va_end(ap);

    lg_write(err->writer, lg_str8_lit("\n"));
}

#define mrv_span_zero_len(offset_) (MRV_Span){ .offset = (offset_) }


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// fundamental lexing data structures
///
////////////////////////////////////////////////////////////////////////////////

#define MRV_DEFINE_TOKEN_KINDS \
    MRV_X(Error,              "") \
    MRV_X(EOF,                "") \
    MRV_X(OpenParen,          "(") \
    MRV_X(CloseParen,         ")") \
    MRV_X(OpenBrace,          "{") \
    MRV_X(CloseBrace,         "}") \
    MRV_X(Colon,              ":") \
    MRV_X(Comma,              ",") \
    MRV_X(Semicolon,          ";") \
    MRV_X(Equals,             "=") \
    MRV_X(BeginHostType,      "<<") \
    MRV_X(EndHostType,        ">>") \
    MRV_X(Unit,               "()") \
    MRV_X(Lambda,             "lambda") \
    MRV_X(Language,           "language") \
    MRV_X(Type,               "type") \
    MRV_X(Operator,           "operator") \
    MRV_X(Combinator,         "combinator") \
    MRV_X(RightArrow,         "->") \
    MRV_X(Ident,              "") \
    MRV_X(SymbolIdent,        "")

typedef int8_t 
MRV_TokenKind;
enum {
#   define MRV_X(kind, ...) MRV_TokenKind_##kind,
    MRV_DEFINE_TOKEN_KINDS
#   undef  MRV_X
};

enum {
#   define MRV_X(...) + 1
    MRV_TokenKind_COUNT = 0 MRV_DEFINE_TOKEN_KINDS
#   undef MRV_X
};

const struct {
    lg_str8 string; 
    lg_str8 kind_string;
    uint32_t hash; 
} 
MRV_TOKEN_TABLE[MRV_TokenKind_COUNT] = {
#   define MRV_X(kind, str) [MRV_TokenKind_##kind] = { \
        .string = lg_str8_lit(str), \
        .kind_string = lg_str8_lit(#kind), \
        .hash = lg_hash_lit_16(str) \
    },
MRV_DEFINE_TOKEN_KINDS
#   undef MRV_X
};

#define mrv_token_kind_is_keyword(kind) (MRV_TOKEN_TABLE[kind].string.len > 1)
#define mrv_token_as_str(kind) (MRV_TOKEN_TABLE[kind].kind_string)
#define mrv_token_assoc_str(kind) (MRV_TOKEN_TABLE[kind].string);

typedef struct
MRV_Token {
    MRV_TokenKind kind;
    MRV_Span span;
} MRV_Token;

typedef struct
MRV_TokenStreamBlock {
    struct MRV_TokenStreamBlock *next;
    struct MRV_TokenStreamBlock *prev;

    MRV_Token  tokens[MRV_TOKEN_STREAM_BLOCK_CAPACITY];
} MRV_TokenStreamBlock;

typedef struct 
MRV_TokenStream {
    size_t tail_len;
    MRV_TokenStreamBlock *tail;
} MRV_TokenStream;

typedef struct
MRV_LexerContext {
    lg_str8        text;
    size_t         current_offset;

    LG_Allocator  *artifact;

    MRV_Error      err;
} MRV_LexerContext;


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// private lexer utils
///
////////////////////////////////////////////////////////////////////////////////

void
mrv_tstream_append(MRV_TokenStream *tstream, LG_Allocator *artifact_allocator, MRV_Token tok) {
    if (lg_likely(
        tstream->tail != NULL &&
        tstream->tail_len < MRV_TOKEN_STREAM_BLOCK_CAPACITY - 1
    )) {
        lg_memcpy(tstream->tail->tokens + tstream->tail_len, &tok, sizeof(MRV_Token));
        tstream->tail_len++;
        return;
    }

    MRV_TokenStreamBlock *next_block = (MRV_TokenStreamBlock*)lg_alloc_zero(artifact_allocator, sizeof(MRV_TokenStreamBlock));
    lg_assert(next_block != NULL);

    if (tstream->tail != NULL) {
        tstream->tail->next = next_block;
    }
    next_block->prev = tstream->tail;

    tstream->tail = next_block;
    tstream->tail_len = 0;

    mrv_tstream_append(tstream, artifact_allocator, tok);
}

void
mrv_tstream_destroy(MRV_TokenStream *tstream, LG_Allocator *artifact_allocator) {
    MRV_TokenStreamBlock *iter_block = tstream->tail;
    while (iter_block != NULL) {
        MRV_TokenStreamBlock *temp = iter_block->prev;
        lg_free(artifact_allocator, iter_block);
        iter_block = temp;
    }
    lg_memzero(tstream, sizeof(MRV_TokenStream));
}

lg_force_inline lg_str8
mrv_span_to_str8(MRV_Span span, lg_str8 text) {
    return (lg_str8){ .len = span.len, .p = text.p + span.offset };
}

lg_force_inline uint8_t
mrv_lexer_peek(MRV_LexerContext *ctx) {
    if (lg_likely(ctx->current_offset < ctx->text.len - 1)) {
        lg_assert(ctx->text.p[ctx->current_offset + 1] != '\0');
        return ctx->text.p[ctx->current_offset + 1];
    }
    return '\0';
}

lg_force_inline void
mrv_lexer_skip(MRV_LexerContext *ctx) {
    if (lg_likely(ctx->current_offset < ctx->text.len)) {
        ctx->current_offset++;
    }
}

lg_force_inline bool
mrv_lexer_match_sequence(MRV_LexerContext *ctx, lg_str8 seq, MRV_Span *lg_nullable out_span) {
    if (ctx->current_offset + seq.len >= ctx->text.len) {
        return false;
    }
    lg_str8 next_n = (lg_str8){ .len = seq.len, .p = &ctx->text.p[ctx->current_offset]};
    if (lg_strcmp(next_n, seq) == 0) {
        if (out_span != NULL) {
            *out_span = (MRV_Span) {
                .offset = ctx->current_offset,
                .len = seq.len,
            };
        }
        ctx->current_offset += seq.len;
        return true;
    }
    return false;
}

lg_force_inline MRV_Token
mrv_lexer_consume_char(MRV_LexerContext *ctx) {
    if (ctx->current_offset >= ctx->text.len) {
        mrv_report_error(&ctx->err, mrv_span_zero_len(ctx->text.len), lg_str8_lit("unexpected EOF"));
        return (MRV_Token){ .kind = MRV_TokenKind_Error };
    }

    uint8_t ch = ctx->text.p[ctx->current_offset];

    MRV_TokenKind kind = MRV_TokenKind_Error;
    for (uint8_t i = 0; i < MRV_TokenKind_COUNT; i++) {
        if (MRV_TOKEN_TABLE[i].string.len != 1) {
            continue;
        }

        if(MRV_TOKEN_TABLE[i].string.p[0] == ch) {
            kind = i;
            break;
        }
    }

    MRV_Token tok = {
        .kind = kind,
        .span.offset = ctx->current_offset,  
        .span.len = 1,
    };

    ctx->current_offset++;

    return tok;
}

lg_force_inline void
mrv_lexer_skip_whitespace(MRV_LexerContext *ctx) {
    for (
        uint8_t ch_i = ctx->text.p[ctx->current_offset];
        lg_char_is_whitespace(ch_i) && ctx->current_offset < ctx->text.len;
        ctx->current_offset++, ch_i = ctx->text.p[ctx->current_offset]
    );
}


lg_force_inline MRV_Token
mrv_lexer_scan_ident(MRV_LexerContext *ctx, MRV_TokenKind expected_kind) {
    MRV_Token tok = { 
        .kind = expected_kind,
        .span.offset = ctx->current_offset,
    };

    /////////////////////////////////////////////////////
    /// ~~ scan sequence ~~

    bool is_first = true;
    while (ctx->current_offset < ctx->text.len) {
        uint8_t ch_i = ctx->text.p[ctx->current_offset];

        if (lg_unlikely(ctx->current_offset >= ctx->text.len)) {
            mrv_report_error(&ctx->err, tok.span, lg_str8_lit("unexpected EOF"));
            return (MRV_Token){ .kind = MRV_TokenKind_Error };
        } 
        if (lg_unlikely(
            is_first &&
            expected_kind == MRV_TokenKind_Ident &&
            lg_char_is_numeric(ch_i)
        )) {
            mrv_report_error(&ctx->err, tok.span, lg_str8_lit("expected letter, found number"));
           return (MRV_Token){ .kind = MRV_TokenKind_Error };
        }
        if (lg_unlikely(!lg_char_is_alphanumeric(ch_i) && ch_i != '_')) {
            break;
        }

        is_first = false;
        ctx->current_offset++;
        tok.span.len++;

    }

    if (tok.span.len == 0) {
        mrv_report_error(&ctx->err, tok.span, lg_str8_lit("expected alphanumeric sequence"));
        return (MRV_Token){ .kind = MRV_TokenKind_Error };
    }
    

    /////////////////////////////////////////////////////
    /// ~~ scan for keywords ~~

    lg_str8 ident_string = mrv_span_to_str8(tok.span, ctx->text);
    uint32_t ident_hash = lg_hash_16(ident_string.p, ident_string.len);

    for (uint8_t kind = 0; kind < MRV_TokenKind_COUNT; kind++) {
        if (!mrv_token_kind_is_keyword(kind)) {
            continue; 
        }
        if (ident_hash == MRV_TOKEN_TABLE[kind].hash) {
            tok.kind = kind;
            break;
        }
    }

    return tok;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// the part that does the lexing
///
////////////////////////////////////////////////////////////////////////////////

MRV_TokenStream
mrv_lex(LG_Allocator *artifact_allocator, lg_str8 text, LG_Writer *err_writer) {
    MRV_LexerContext ctx = {
        .artifact = artifact_allocator,
        .text = text,
        .err.writer = err_writer,
    };
    MRV_TokenStream tstream = {0};

    while (ctx.current_offset < ctx.text.len) {
        mrv_lexer_skip_whitespace(&ctx);

        switch (ctx.text.p[ctx.current_offset]) {
        case 'A'...'Z':
        case 'a'...'z': {
            MRV_Token sym_ident = mrv_lexer_scan_ident(&ctx, MRV_TokenKind_Ident);
            mrv_tstream_append(&tstream, ctx.artifact, sym_ident);

            break; 
        }

        case '%': {
            mrv_lexer_skip(&ctx);

            MRV_Token sym_ident = mrv_lexer_scan_ident(&ctx, MRV_TokenKind_SymbolIdent);
            mrv_tstream_append(&tstream, ctx.artifact, sym_ident);

            break;
        }

        case '-': {
            uint8_t next_ch = mrv_lexer_peek(&ctx);
            if (next_ch == '>') {
                MRV_Token tok = {
                    .kind = MRV_TokenKind_RightArrow,
                    .span.offset = ctx.current_offset,  
                    .span.len = 2,
                };
                ctx.current_offset += 2;
                mrv_tstream_append(&tstream, ctx.artifact, tok);
            } else {
                goto unexpected_char;
            }

            break;
        }


        case '(': {
            MRV_Span span = {0};
            if (mrv_lexer_match_sequence(&ctx, lg_str8_lit("(*"), NULL)) {
                mrv_lexer_skip(&ctx);
                mrv_lexer_skip(&ctx);
                const lg_str8 close = lg_str8_lit("*)");
                while (!mrv_lexer_match_sequence(&ctx, close, NULL)) {
                    mrv_lexer_skip(&ctx);
                }
                mrv_lexer_skip(&ctx);
            } else if (mrv_lexer_match_sequence(&ctx, lg_str8_lit("()"), &span)) {
                mrv_tstream_append(&tstream, artifact_allocator, (MRV_Token){ .span = span, .kind = MRV_TokenKind_Unit });
            } else {
                goto single_char;
            }

            break;
        }

        case '<': {
            MRV_Span span = {0};
            if (!mrv_lexer_match_sequence(&ctx, lg_str8_lit("<<"), &span)) {
                goto unexpected_char;
            }
            mrv_tstream_append(&tstream, artifact_allocator, (MRV_Token){
                .kind = MRV_TokenKind_BeginHostType,
                .span = span,
            });
            break;
        }

        case '>': {
            MRV_Span span = {0};
            const lg_str8 close = lg_str8_lit(">>");
            if (!mrv_lexer_match_sequence(&ctx, close, &span)) {
                goto unexpected_char;
            }
            mrv_tstream_append(&tstream, artifact_allocator, (MRV_Token){
                .kind = MRV_TokenKind_EndHostType,
                .span = span,
            });
            break;
        }

single_char:
        case ')':
        case '{':
        case '}':
        case ':':
        case ',':
        case ';':
        case '=': {
            MRV_Token ch = mrv_lexer_consume_char(&ctx);
            mrv_tstream_append(&tstream, ctx.artifact, ch);

            break;
        }

        case '\0':
            mrv_lexer_skip(&ctx);
            break;

unexpected_char:;
        default: {
            MRV_Span err_span = (MRV_Span){ .len = 1, .offset = ctx.current_offset };
            lg_str8 unexpected_char = mrv_span_to_str8(err_span, ctx.text);

            mrv_tstream_append(&tstream, ctx.artifact, (MRV_Token){
                .kind = MRV_TokenKind_Error,
                .span = err_span,
            });
            mrv_report_error(&ctx.err, err_span, lg_str8_lit("unexpected character: %{str}"), unexpected_char);

            mrv_lexer_skip(&ctx);
        }
        }
    }

    mrv_tstream_append(&tstream, artifact_allocator, (MRV_Token){
        .kind = MRV_TokenKind_EOF,
        .span = mrv_span_zero_len(ctx.current_offset),
    });

    return tstream;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// fundamental parsing data structures
///
////////////////////////////////////////////////////////////////////////////////

// the below parsing code is some of the grossest code on planet earth.
// change things with caution.

typedef uint8_t
MRV_ParserStatusKind;
enum {
    MRV_ParserStatusKind_OK,
    MRV_ParserStatusKind_NOK,
};

#define MRV_DEFINE_AST_NODE_KINDS \
    MRV_X(Error) \
    MRV_X(Program) \
    MRV_X(SymbolIdent) \
    MRV_X(HostTypeIdent) \
    MRV_X(OtherIdent) \
    MRV_X(Unit) \
    MRV_X(SymbolDeclaration) \
    MRV_X(LanguageDeclaration) \
    MRV_X(CombinatorDeclaration) \
    MRV_X(TypeDeclaration) \
    MRV_X(OperatorDeclaration) \
    MRV_X(NonTrivialType) \
    MRV_X(DeclarationArg) \
    MRV_X(DeclarationArgList) \
    MRV_X(InvocationArgList) \
    MRV_X(ExpressionStatement) \
    MRV_X(AssignmentStatement) \
    MRV_X(InvocationExpression) \
    MRV_X(LambdaExpression) \
    MRV_X(Block) \

typedef uint8_t
MRV_ASTNodeKind;
enum 
MRV_ASTNodeKind {
#   define MRV_X(kind, ...) MRV_ASTNodeKind_##kind,
    MRV_DEFINE_AST_NODE_KINDS
#   undef MRV_X
};

enum {
#   define MRV_X(...) + 1
    MRV_ASTNodeKind_COUNT = 0 MRV_DEFINE_AST_NODE_KINDS
#   undef MRV_X
};

lg_str8
MRV_AST_NODE_STRINGS[MRV_ASTNodeKind_COUNT] = {
#   define MRV_X(kind, ...) [MRV_ASTNodeKind_##kind] = lg_str8_lit(#kind),
    MRV_DEFINE_AST_NODE_KINDS
#   undef MRV_X
};

#define mrv_ast_node_kind_as_str(kind) MRV_AST_NODE_STRINGS[(kind)]
#define mrv_match_ast_node(kind) switch((enum MRV_ASTNodeKind)kind)

typedef struct MRV_ASTNode MRV_ASTNode;

typedef union 
MRV_ASTNodeChildren {
    struct {} Error;

    struct {
        size_t         n_children;
        MRV_ASTNode  **children;
    } Program;


    struct {} SymbolIdent;
    struct {} HostTypeIdent;
    struct {} OtherIdent;
    struct {} Unit;
 
    struct {
        MRV_ASTNode *ident;
        MRV_ASTNode *arg_list;
    } InvocationExpression;

    struct {
        MRV_ASTNode *decl_arg_list;
        MRV_ASTNode *body_block;
    } LambdaExpression;

    struct {
        MRV_ASTNode *symbol_ident;
        MRV_ASTNode *type_ident;
    } SymbolDeclaration;

    struct {
        MRV_ASTNode *ident;
    } LanguageDeclaration;

    struct {
        MRV_ASTNode *ident;
        MRV_ASTNode *non_trivial_alias;
    } TypeDeclaration;

    struct {
        MRV_ASTNode *outermost_ident;
        MRV_ASTNode *invocation_arg_list;
    } NonTrivialType;

    struct {
        MRV_ASTNode *ident;
        MRV_ASTNode *arg_list;
        MRV_ASTNode *return_type;
    } OperatorDeclaration;

    struct {
        MRV_ASTNode *ident;
        MRV_ASTNode *type;
    } DeclarationArg;

    struct {
        size_t         n_args;
        MRV_ASTNode  **args;
    } DeclarationArgList;

    struct {
        size_t         n_args;
        MRV_ASTNode  **args;
    } InvocationArgList;
    
    struct {
        MRV_ASTNode *ident;
        MRV_ASTNode *arg_list;
        MRV_ASTNode *body;
    } CombinatorDeclaration;

    struct {
        size_t         n_statements;
        MRV_ASTNode  **statements;
    } Block;

    struct {
        MRV_ASTNode *symbol_decl;
        MRV_ASTNode *expression;
    } AssignmentStatement;

    struct {
        MRV_ASTNode *expression;
    } ExpressionStatement;
} MRV_ASTNodeChildren;

struct 
MRV_ASTNode {
    MRV_ASTNodeChildren children_as;
    MRV_ASTNodeKind kind;
    MRV_Span span;
};

typedef struct
MRV_ASTNodeRefStack {
    struct MRV_ASTNodeRefStack *prev;
    MRV_ASTNode *to;
} MRV_ASTNodeRefStack;

typedef struct
MRV_ParserContext {
    lg_str8                text;

    MRV_ASTNodeRefStack   *ref_stack_top;

    MRV_TokenStream       *tstream;
    MRV_TokenStreamBlock  *cur_block;
    size_t                 next_offset;

    MRV_Error              err;
    LG_Arena               scratch;

    LG_Arena               artifact;
    MRV_ASTNode           *nil_node;
} MRV_ParserContext;

typedef struct
MRV_AST {
    MRV_ASTNode  *nil_node;
    MRV_ASTNode  *root;
    LG_Arena      artifact;
} MRV_AST;

#define mrv_parser_has_err(ctx) ((ctx)->err.is_err)


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// iterate over a token stream
///
////////////////////////////////////////////////////////////////////////////////

lg_force_inline bool
mrv_parser_is_end(MRV_ParserContext *ctx) {
    lg_assert(ctx->cur_block != NULL);

    return (
        ctx->cur_block->next == NULL &&
        ctx->tstream->tail == ctx->cur_block &&
        ctx->next_offset >= ctx->tstream->tail_len
    );
}

lg_force_inline MRV_ParserStatusKind 
mrv_parser_find_next(
    MRV_ParserContext *ctx,
    size_t *out_next_offset,
    MRV_TokenStreamBlock **out_next_block
) {
    lg_assert(ctx != NULL);
    lg_assert(out_next_block != NULL);
    lg_assert(out_next_offset != NULL);

    const size_t cur_len = ctx->cur_block == ctx->tstream->tail ? 
        ctx->tstream->tail_len :
        MRV_TOKEN_STREAM_BLOCK_CAPACITY;

    if (lg_likely(ctx->next_offset < cur_len)) {
        *out_next_block = ctx->cur_block;
        *out_next_offset = ctx->next_offset + 1;
        return MRV_ParserStatusKind_OK;
    }

    if (mrv_parser_is_end(ctx)) {
        return MRV_ParserStatusKind_NOK;
    }

    *out_next_block = ctx->cur_block->next;
    *out_next_offset = 0;

    return MRV_ParserStatusKind_OK;
}

MRV_Token
mrv_parser_consume(MRV_ParserContext *ctx) {
    lg_assert(ctx != NULL);

    size_t next_offset;
    MRV_TokenStreamBlock *next_block;
    MRV_ParserStatusKind status = mrv_parser_find_next(ctx, &next_offset, &next_block);
    lg_assert(status == MRV_ParserStatusKind_OK); // this function shouldn't be called where there isn't a next token
                         // e.g don't expect a token after an EOF
                         // TODO: this assumption may case nodes with variadic children left unterminated
                         // to crash the parser

    MRV_Token ret = ctx->cur_block->tokens[ctx->next_offset];
    ctx->next_offset = next_offset;
    ctx->cur_block = next_block;

    return ret;
}

MRV_Token
mrv_parser_expect(
    MRV_ParserContext *ctx,
    MRV_TokenKind expected_kind
) {
    lg_assert(ctx != NULL);
    lg_assert(expected_kind != MRV_TokenKind_Error);

    MRV_Token next_token = mrv_parser_consume(ctx);

    if (expected_kind != next_token.kind) {
        mrv_report_error(
            &ctx->err,
            next_token.span,
            lg_str8_lit("expected %{str}, found string \"%{str}\" of token kind %{str}"), 
            mrv_token_as_str(expected_kind),    
            mrv_span_to_str8(next_token.span, ctx->text),
            mrv_token_as_str(next_token.kind)
        );
        return lg_nil(MRV_Token);        
    }

    return next_token;
}

MRV_Token
mrv_parser_peek(MRV_ParserContext *ctx) {
    return ctx->cur_block->tokens[ctx->next_offset];
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// rdp helpers
///
////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
/// note: with these nrs helpers, you still have to use lg_push/pop_scope

lg_force_inline MRV_ASTNode*
mrv_parser_nil_node(MRV_ParserContext *ctx) {
    lg_assert(ctx != NULL);
    lg_assert(ctx->nil_node != NULL);
    lg_memzero(ctx->nil_node, sizeof(MRV_ASTNode));
    return ctx->nil_node;
}

lg_force_inline bool
mrv_parser_is_nil_node(MRV_ParserContext *ctx, MRV_ASTNode *node) {
    lg_assert(ctx != NULL);
    lg_assert(ctx->nil_node != NULL);
    return node == ctx->nil_node;
}

lg_force_inline void
mrv_parser_nrs_push(MRV_ParserContext *ctx, MRV_ASTNode *to) {
    MRV_ASTNodeRefStack* to_push = (MRV_ASTNodeRefStack*)lg_arena_alloc_struct(&ctx->scratch, MRV_ASTNodeRefStack);
    lg_assert(to_push != NULL);

    to_push->to = to;

    if (ctx->ref_stack_top != NULL) {
        to_push->prev = ctx->ref_stack_top;
    }
    ctx->ref_stack_top = to_push;
}

lg_force_inline MRV_ASTNode**
mrv_parser_nrs_unwind_cpy(MRV_ParserContext *ctx, uint32_t n_refs) {
    if (n_refs == 0) {
        return NULL;
    }

    MRV_ASTNode **refs = lg_arena_alloc_array(&ctx->artifact, MRV_ASTNode*, n_refs);
    lg_assert(refs != NULL);

    uint32_t i = 0;
    while (ctx->ref_stack_top != NULL && i < n_refs) {
        MRV_ASTNode *next = ctx->ref_stack_top->to;
        refs[i] = next;
        ctx->ref_stack_top = ctx->ref_stack_top->prev;
        i++;
    }

    // since they were inserted in stack order, they'll be in reverse-source order
    for (size_t i = 0; i < n_refs / 2; i++) {
        MRV_ASTNode *temp = refs[i];
        refs[i] = refs[n_refs - 1 - i];
        refs[n_refs - 1 - i] = temp;
    }

    return refs;
}

#define mrv_parser_mknode(ctx, kind, span, ...) mrv_parser_mknode_( \
    ctx, \
    MRV_ASTNodeKind_##kind, \
    span, \
    (MRV_ASTNodeChildren){ .kind = {__VA_ARGS__} } \
)

lg_force_inline MRV_ASTNode*
mrv_parser_mknode_(MRV_ParserContext *ctx, MRV_ASTNodeKind kind, MRV_Span span, MRV_ASTNodeChildren children) {
    MRV_ASTNode *node = lg_arena_alloc_struct(&ctx->artifact, MRV_ASTNode);
    lg_assert(node != NULL);

    node->kind = kind;
    node->span = span;
    node->children_as = children;

    return node;
}

lg_force_inline MRV_Span
mrv_get_bounding_span(MRV_ParserContext *ctx, size_t n_nodes, MRV_ASTNode **nodes) {
    if (n_nodes == 0) {
        return lg_nil(MRV_Span);
    }

    size_t min_offset = SIZE_MAX,
           max_offset = 0;
    for (size_t i = 0; i < n_nodes; i++) {
        lg_assert(nodes[i] != NULL);

        if (mrv_parser_is_nil_node(ctx, nodes[i])) {
            continue;
        }

        if (nodes[i]->span.offset < min_offset) {
            min_offset = nodes[i]->span.offset;
        }

        size_t offset = nodes[i]->span.offset + nodes[i]->span.len;
        if (offset > max_offset) {
            max_offset = offset;
        }
    }

    return (MRV_Span){ .offset = min_offset, .len = max_offset - min_offset };
}

lg_force_inline void
mrv_parser_unexpected_token(MRV_ParserContext *ctx, MRV_Token tok, MRV_ASTNodeKind parent) {
    mrv_report_error(
        &ctx->err,
        tok.span,
        lg_str8_lit("unexpected token: \"%{str}\" of token kind %{str} in %{str} node"),
        mrv_span_to_str8(tok.span, ctx->text),
        mrv_token_as_str(tok.kind),
        mrv_ast_node_kind_as_str(parent)
    );
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// actual rdp procedures
///
////////////////////////////////////////////////////////////////////////////////

MRV_ASTNode*
mrv_parse_other_ident(MRV_ParserContext *ctx) {
    MRV_Token tok = mrv_parser_expect(ctx, MRV_TokenKind_Ident);
    MRV_ASTNode *node = mrv_parser_mknode(ctx, OtherIdent, tok.span);
    return node;
}

MRV_ASTNode*
mrv_parse_host_type_ident(MRV_ParserContext *ctx) {
    MRV_Token ident = mrv_parser_expect(ctx, MRV_TokenKind_Ident);
    mrv_parser_expect(ctx, MRV_TokenKind_EndHostType);
    MRV_ASTNode *node = mrv_parser_mknode(ctx, HostTypeIdent, ident.span);
    return node;
}

MRV_ASTNode*
mrv_parse_symbol_ident(MRV_ParserContext *ctx) {
    MRV_Token tok = mrv_parser_expect(ctx, MRV_TokenKind_SymbolIdent);
    MRV_ASTNode *node = mrv_parser_mknode(ctx, SymbolIdent, tok.span);
    return node;
}

MRV_ASTNode*
mrv_parse_unit(MRV_ParserContext *ctx) {
    MRV_Token tok = mrv_parser_expect(ctx, MRV_TokenKind_Unit);
    MRV_ASTNode *node = mrv_parser_mknode(ctx, Unit, tok.span);
    return node;
}

MRV_ASTNode*
mrv_parse_symbol_decl(MRV_ParserContext *ctx) {
    MRV_ASTNode *symbol_ident = mrv_parse_symbol_ident(ctx);
    mrv_parser_expect(ctx, MRV_TokenKind_Colon);
    MRV_ASTNode *type_ident = mrv_parse_other_ident(ctx);

    MRV_Span all_span = mrv_get_bounding_span(ctx, 2, (MRV_ASTNode*[]){symbol_ident, type_ident});
    MRV_ASTNode *node = mrv_parser_mknode(ctx, SymbolDeclaration, all_span, symbol_ident, type_ident);

    return node;
}

MRV_ASTNode*
mrv_parse_decl_arg(MRV_ParserContext *ctx) {
    MRV_Token peek = mrv_parser_peek(ctx);
    
    MRV_ASTNode *name;
    if (peek.kind == MRV_TokenKind_Ident) {
        name = mrv_parse_other_ident(ctx);
    } else {
        name = mrv_parse_symbol_ident(ctx);
    }

    mrv_parser_expect(ctx, MRV_TokenKind_Colon);

    MRV_ASTNode *type;
    peek = mrv_parser_peek(ctx);
    if (peek.kind == MRV_TokenKind_BeginHostType) {
        mrv_parser_consume(ctx);
        type = mrv_parse_host_type_ident(ctx);
    } else {
        type = mrv_parse_other_ident(ctx);
    }
    
    MRV_Span all_span = mrv_get_bounding_span(ctx, 2, (MRV_ASTNode*[]){name, type});
    MRV_ASTNode *node = mrv_parser_mknode(ctx, DeclarationArg, all_span, .ident = name, .type = type);

    return node;
}

MRV_ASTNode*
mrv_parse_decl_arg_list(MRV_ParserContext *ctx, bool is_binary) {
    LG_Scope scope = lg_push_scope(&ctx->scratch);

    uint32_t n_children = 0;

    while (true) {
        MRV_Token peek = mrv_parser_peek(ctx);
        switch (peek.kind) {
        case MRV_TokenKind_CloseParen:
            mrv_parser_consume(ctx);
            goto loop_end;
        case MRV_TokenKind_Comma:
            mrv_parser_consume(ctx);
            break;
        case MRV_TokenKind_Ident:
        case MRV_TokenKind_SymbolIdent: {
            MRV_ASTNode *arg = mrv_parse_decl_arg(ctx);
            mrv_parser_nrs_push(ctx, arg);
            n_children++;
            break;
        }
        default:
            mrv_parser_unexpected_token(ctx, peek, MRV_ASTNodeKind_DeclarationArgList);
            mrv_parser_consume(ctx);
            goto loop_end;
        }
    }
loop_end:;

    MRV_ASTNode **children = mrv_parser_nrs_unwind_cpy(ctx, n_children);
    MRV_Span all_span = mrv_get_bounding_span(ctx, n_children, children);

    if (n_children > 2 && is_binary) {
        mrv_report_error(&ctx->err, all_span, lg_str8_lit(
            "all operators must be pure three-address code\n"
            "this argument list has %{i64} arguments"
        ), n_children);
        return mrv_parser_nil_node(ctx);
    }

    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        DeclarationArgList,
        all_span,
        .args = children,
        .n_args = n_children
    );

    lg_pop_scope(&ctx->scratch, scope);

    return node;
}

MRV_ASTNode*
mrv_parse_invocation_arg_list(MRV_ParserContext *ctx) {
    LG_Scope scope = lg_push_scope(&ctx->scratch);

    uint32_t n_children = 0;

    while (true) {
        MRV_Token peek = mrv_parser_peek(ctx);
        switch (peek.kind) {
        case MRV_TokenKind_CloseParen:
            mrv_parser_consume(ctx);
            goto loop_end;

        case MRV_TokenKind_Comma:
            mrv_parser_consume(ctx);
            break;

        case MRV_TokenKind_Ident: {
            MRV_ASTNode *arg = mrv_parse_other_ident(ctx);
            mrv_parser_nrs_push(ctx, arg);
            n_children++;
            break;
        }

        case MRV_TokenKind_SymbolIdent: {
            MRV_ASTNode *arg = mrv_parse_symbol_ident(ctx);
            mrv_parser_nrs_push(ctx, arg);
            n_children++;
            break;
        }

        case MRV_TokenKind_Unit: {
            MRV_ASTNode *unit = mrv_parse_unit(ctx);
            mrv_parser_nrs_push(ctx, unit);
            n_children++;
            break;
        }
        
        default:
            mrv_parser_unexpected_token(ctx, peek, MRV_ASTNodeKind_InvocationArgList);
            mrv_parser_consume(ctx);
            goto loop_end;
        }
    }
loop_end:;

    MRV_ASTNode **children = mrv_parser_nrs_unwind_cpy(ctx, n_children);
    MRV_Span all_span = mrv_get_bounding_span(ctx, n_children, children);
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        InvocationArgList,
        all_span,
        .args = children,
        .n_args = n_children
    );

    lg_pop_scope(&ctx->scratch, scope);

    return node;

}

// forward decl b/c blocks can contain blocks 
MRV_ASTNode*
mrv_parse_block(MRV_ParserContext *ctx);

MRV_ASTNode*
mrv_parse_invocation_expr(MRV_ParserContext *ctx) {
    MRV_ASTNode *op_ident = mrv_parse_other_ident(ctx);
    mrv_parser_expect(ctx, MRV_TokenKind_OpenParen);

    MRV_ASTNode *arg_list = mrv_parse_invocation_arg_list(ctx);

    MRV_Span all_span = mrv_get_bounding_span(ctx, 2, (MRV_ASTNode*[]){op_ident, arg_list});
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        InvocationExpression,
        all_span,
        .ident = op_ident,
        .arg_list = arg_list,
    );

    return node;
}

MRV_ASTNode*
mrv_parse_expr_statement(MRV_ParserContext *ctx) {
    MRV_ASTNode *expr = mrv_parse_invocation_expr(ctx);
    mrv_parser_expect(ctx, MRV_TokenKind_Semicolon);
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        ExpressionStatement,
        expr->span,
        .expression = expr,
    );
    return node;
}

MRV_ASTNode*
mrv_parse_lambda_expr(MRV_ParserContext *ctx) {
    mrv_parser_expect(ctx, MRV_TokenKind_OpenParen);
    MRV_ASTNode *decl_arg_list = mrv_parse_decl_arg_list(ctx, true);
    mrv_parser_expect(ctx, MRV_TokenKind_OpenBrace);
    MRV_ASTNode *body_block = mrv_parse_block(ctx);

    MRV_Span all_span = mrv_get_bounding_span(ctx, 2, (MRV_ASTNode*[]){decl_arg_list, body_block});
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        LambdaExpression,
        all_span,
        .decl_arg_list = decl_arg_list,
        .body_block = body_block,
    );

    return node;
}

MRV_ASTNode*
mrv_parse_assignment_statement(MRV_ParserContext *ctx) {
    MRV_ASTNode *symbol_decl = mrv_parse_symbol_decl(ctx);
    mrv_parser_expect(ctx, MRV_TokenKind_Equals);

    MRV_Token peek = mrv_parser_peek(ctx);

    MRV_ASTNode *expr;
    if (peek.kind == MRV_TokenKind_Lambda) {
        mrv_parser_consume(ctx);
        expr = mrv_parse_lambda_expr(ctx);
    } else {
        expr = mrv_parse_invocation_expr(ctx);
    }
    mrv_parser_expect(ctx, MRV_TokenKind_Semicolon);

    MRV_Span all_span = mrv_get_bounding_span(ctx, 2, (MRV_ASTNode*[]){symbol_decl, expr});
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        AssignmentStatement,
        all_span,
        .symbol_decl = symbol_decl,
        .expression = expr,
    );
    
    return node; 
}

MRV_ASTNode*
mrv_parse_block(MRV_ParserContext *ctx) {
    LG_Scope scope = lg_push_scope(&ctx->scratch);

    uint32_t n_children = 0;

    while (true) {
        MRV_Token peek = mrv_parser_peek(ctx);

        switch (peek.kind) {
        case MRV_TokenKind_CloseBrace: {
            mrv_parser_consume(ctx);
            goto loop_end;
        }

        case MRV_TokenKind_Ident: {
            MRV_ASTNode *stmt = mrv_parse_expr_statement(ctx);
            mrv_parser_nrs_push(ctx, stmt);
            n_children++;
            break;
        }

        case MRV_TokenKind_SymbolIdent: {
            MRV_ASTNode *stmt = mrv_parse_assignment_statement(ctx);
            mrv_parser_nrs_push(ctx, stmt);
            n_children++;
            break;
        }

        default:
            mrv_parser_unexpected_token(ctx, peek, MRV_ASTNodeKind_Block);
            mrv_parser_consume(ctx);
            goto loop_end;
        }
    }
loop_end:;

    MRV_ASTNode **children = mrv_parser_nrs_unwind_cpy(ctx, n_children);
    MRV_Span all_span = mrv_get_bounding_span(ctx, n_children, children);
    MRV_ASTNode *node = mrv_parser_mknode(ctx, Block, all_span, .n_statements = n_children, .statements = children);

    lg_pop_scope(&ctx->scratch, scope);
    return node;
}

MRV_ASTNode*
mrv_parse_combinator_decl(MRV_ParserContext *ctx) {
    MRV_ASTNode *ident = mrv_parse_other_ident(ctx);

    mrv_parser_expect(ctx, MRV_TokenKind_OpenParen);
    MRV_ASTNode *arg_list = mrv_parse_decl_arg_list(ctx, false);

    mrv_parser_expect(ctx, MRV_TokenKind_OpenBrace);
    MRV_ASTNode *body = mrv_parse_block(ctx);

    if (mrv_parser_has_err(ctx)) {
        return mrv_parser_nil_node(ctx);
    }

    MRV_Span all_span = mrv_get_bounding_span(ctx, 3, (MRV_ASTNode*[]){ident, arg_list, body});
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        CombinatorDeclaration,
        all_span,
        .ident = ident,
        .arg_list = arg_list,
        .body = body,
    );

    return node;
}

MRV_ASTNode*
mrv_parse_language_decl(MRV_ParserContext *ctx) {
    MRV_ASTNode *ident = mrv_parse_other_ident(ctx);
    mrv_parser_expect(ctx, MRV_TokenKind_Semicolon);
    MRV_ASTNode *node = mrv_parser_mknode(ctx, LanguageDeclaration, ident->span, .ident = ident);
    return node;
}

MRV_ASTNode*
mrv_parse_non_trivial_type(MRV_ParserContext *ctx) {
    MRV_ASTNode *outermost_ident = mrv_parse_other_ident(ctx);
    mrv_parser_expect(ctx, MRV_TokenKind_OpenParen);
    MRV_ASTNode *arg_list = mrv_parse_invocation_arg_list(ctx);
    MRV_Span all_span = mrv_get_bounding_span(ctx, 2, (MRV_ASTNode*[]){outermost_ident, arg_list});
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        NonTrivialType,
        all_span,
        .outermost_ident = outermost_ident,
        .invocation_arg_list = arg_list,
    );
    return node;
}

MRV_ASTNode*
mrv_parse_type_decl(MRV_ParserContext *ctx) {
    MRV_ASTNode *ident = mrv_parse_other_ident(ctx);
    MRV_ASTNode *non_trivial_alias = mrv_parser_nil_node(ctx);

    MRV_Token peek = mrv_parser_peek(ctx);
    if (peek.kind == MRV_TokenKind_Equals) {
        mrv_parser_consume(ctx);
        non_trivial_alias = mrv_parse_non_trivial_type(ctx);
    }

    mrv_parser_expect(ctx, MRV_TokenKind_Semicolon);

    MRV_Span all_span = mrv_get_bounding_span(ctx, 2, (MRV_ASTNode*[]){ident, non_trivial_alias});
    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        TypeDeclaration,
        all_span, 
        .ident = ident,
        .non_trivial_alias = non_trivial_alias,
    );

    return node;
}

MRV_ASTNode*
mrv_parse_operator_decl(MRV_ParserContext *ctx) {
    MRV_ASTNode *ident = mrv_parse_other_ident(ctx);
    mrv_parser_expect(ctx, MRV_TokenKind_OpenParen);

    MRV_ASTNode *arg_list = mrv_parse_decl_arg_list(ctx, true);

    MRV_ASTNode *return_type = mrv_parser_nil_node(ctx);
    MRV_Token peek = mrv_parser_peek(ctx);
    if (peek.kind == MRV_TokenKind_RightArrow) {
        mrv_parser_consume(ctx);
        return_type = mrv_parse_other_ident(ctx);
    }

    mrv_parser_expect(ctx, MRV_TokenKind_Semicolon);

    MRV_ASTNode *node = mrv_parser_mknode(
        ctx,
        OperatorDeclaration,
        ident->span,
        .ident = ident,
        .arg_list = arg_list,
        .return_type = return_type,
    );

    return node;
}

MRV_ASTNode*
mrv_parse_program(MRV_ParserContext *ctx) {
    LG_Scope scope = lg_push_scope(&ctx->scratch);
    MRV_ASTNode *root = mrv_parser_nil_node(ctx);
    size_t n_children = 0;

    MRV_Token tok = mrv_parser_peek(ctx);
    while (true) {
        switch (tok.kind) {
        case MRV_TokenKind_EOF: {
            goto loop_end;
        }

        case MRV_TokenKind_Combinator: {
            mrv_parser_consume(ctx);
            MRV_ASTNode *func = mrv_parse_combinator_decl(ctx);
            mrv_parser_nrs_push(ctx, func);
            break;
        }

        case MRV_TokenKind_Language: {
            mrv_parser_consume(ctx);
            MRV_ASTNode *language_decl = mrv_parse_language_decl(ctx);
            mrv_parser_nrs_push(ctx, language_decl);
            break;
        }

        case MRV_TokenKind_Type: {
            mrv_parser_consume(ctx);
            MRV_ASTNode *type_decl = mrv_parse_type_decl(ctx);
            mrv_parser_nrs_push(ctx, type_decl);
            break;
        }

        case MRV_TokenKind_Operator: {
            mrv_parser_consume(ctx);
            MRV_ASTNode *operator_decl = mrv_parse_operator_decl(ctx);
            mrv_parser_nrs_push(ctx, operator_decl);
            break;
        }

        case MRV_TokenKind_Error: 
            lg_unreachable();

        default: {
            mrv_parser_unexpected_token(ctx, tok, MRV_ASTNodeKind_Program);
            mrv_parser_consume(ctx);
            root = mrv_parser_mknode(ctx, Error, tok.span);
            goto out;
        }
        }

        n_children++;
        tok = mrv_parser_peek(ctx);
    }
loop_end:;

    MRV_ASTNode **children = mrv_parser_nrs_unwind_cpy(ctx, n_children);
    MRV_Span all_span = mrv_get_bounding_span(ctx, n_children, children);
    root = mrv_parser_mknode(ctx, Program, all_span, .n_children = n_children, children = children);
    lg_assert(root != NULL);

    root->kind = MRV_ASTNodeKind_Program;
    root->span = tok.span;

out:
    lg_pop_scope(&ctx->scratch, scope);
    return root;
}

MRV_AST
mrv_parse(
    LG_Allocator *artifact_allocator,
    LG_Allocator *scratch_allocator, 
    LG_Writer *err_writer,
    MRV_TokenStream *tstream,
    lg_str8 text
) {
    lg_assert(artifact_allocator != NULL);
    lg_assert(scratch_allocator != NULL);
    lg_assert(tstream != NULL);
    lg_assert(tstream->tail->next == NULL);

    
    /////////////////////////////////////
    /// ~~ initialize the parser ~~

    MRV_ParserContext ctx = {
        .tstream = tstream,
        .err.writer = err_writer,
        .text = text,
    };

    // this will loop forever if there are cycles
    MRV_TokenStreamBlock *iter_block = ctx.tstream->tail;
    while (iter_block != NULL) {
        ctx.cur_block = iter_block;
        iter_block = iter_block->prev;
    }

    lg_arena_init(&ctx.artifact, artifact_allocator);
    lg_arena_init(&ctx.scratch, scratch_allocator);

    ctx.nil_node = lg_arena_alloc_struct(&ctx.artifact, MRV_ASTNode);
    lg_assert(ctx.nil_node != NULL);


    /////////////////////////////////////
    /// ~~ do the parsing ~~

    MRV_ASTNode *root = mrv_parse_program(&ctx);
    MRV_AST ast = {
        .root = root,
        .nil_node = ctx.nil_node,
        .artifact = ctx.artifact,
    };

    lg_arena_free_all(&ctx.scratch);

    return ast;
}

void
mrv_ast_destroy(MRV_AST *ast) {
    lg_arena_free_all(&ast->artifact);
    lg_memzero(ast, sizeof(MRV_AST));
}

lg_force_inline bool
mrv_ast_is_root(MRV_AST *ast, MRV_ASTNode *node) {
    lg_assert(ast != NULL);
    lg_assert(ast->root != NULL);
    return node == ast->root;
}

lg_force_inline bool
mrv_ast_is_nil_node(MRV_AST *ast, MRV_ASTNode *node) {
    lg_assert(ast != NULL);
    lg_assert(ast->nil_node != NULL);
    return node == ast->nil_node;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// dump the ast
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
MRV_ASTDumpContext {
    MRV_AST    *ast;
    LG_Writer  *writer;
    lg_str8     text;
    int8_t      indent;
} MRV_ASTDumpContext;

lg_force_inline void
mrv_ast_dump_indent(MRV_ASTDumpContext *ctx) {
    for (int8_t i = 0; i < ctx->indent; i++) {
        lg_write(ctx->writer, lg_str8_lit("    "));
    }
}

void
mrv_ast_dump_r(MRV_ASTDumpContext *ctx, MRV_ASTNode *lg_nullable parent, MRV_ASTNode *self) {
    if (mrv_ast_is_nil_node(ctx->ast, self)) {
        return;
    }

    bool is_leaf = false;
    MRV_ASTNodeChildren as = self->children_as;

    mrv_match_ast_node (self->kind) {
        case MRV_ASTNodeKind_Error:
        case MRV_ASTNodeKind_OtherIdent:
        case MRV_ASTNodeKind_SymbolIdent:
        case MRV_ASTNodeKind_HostTypeIdent:
        case MRV_ASTNodeKind_Unit:
            is_leaf = true;
            break;

        case MRV_ASTNodeKind_LanguageDeclaration:
            mrv_ast_dump_r(ctx, self, as.LanguageDeclaration.ident);
            break;

        case MRV_ASTNodeKind_TypeDeclaration:
            mrv_ast_dump_r(ctx, self, as.TypeDeclaration.ident);
            mrv_ast_dump_r(ctx, self, as.TypeDeclaration.non_trivial_alias);
            break;

        case MRV_ASTNodeKind_NonTrivialType:
            mrv_ast_dump_r(ctx, self, as.NonTrivialType.invocation_arg_list);
            mrv_ast_dump_r(ctx, self, as.NonTrivialType.outermost_ident);
            break;

        case MRV_ASTNodeKind_OperatorDeclaration:
            mrv_ast_dump_r(ctx, self, as.OperatorDeclaration.ident);
            mrv_ast_dump_r(ctx, self, as.OperatorDeclaration.arg_list);
            mrv_ast_dump_r(ctx, self, as.OperatorDeclaration.return_type);
            break;

        case MRV_ASTNodeKind_Program:
            for (uint32_t i = 0; i < as.Program.n_children; i++) {
                mrv_ast_dump_r(ctx, self, as.Program.children[i]);
            }
            break;

        case MRV_ASTNodeKind_SymbolDeclaration:
            mrv_ast_dump_r(ctx, self, as.SymbolDeclaration.symbol_ident);
            mrv_ast_dump_r(ctx, self, as.SymbolDeclaration.type_ident);
            break;

        case MRV_ASTNodeKind_InvocationArgList:
            for (uint32_t i = 0; i < as.DeclarationArgList.n_args; i++) {
                mrv_ast_dump_r(ctx, self, as.DeclarationArgList.args[i]);
            }
            break;

        case MRV_ASTNodeKind_InvocationExpression:
            mrv_ast_dump_r(ctx, self, as.InvocationExpression.ident);
            mrv_ast_dump_r(ctx, self, as.InvocationExpression.arg_list);
            break;

        case MRV_ASTNodeKind_LambdaExpression:
            mrv_ast_dump_r(ctx, self, as.LambdaExpression.body_block);
            mrv_ast_dump_r(ctx, self, as.LambdaExpression.decl_arg_list);
            break;

        case MRV_ASTNodeKind_CombinatorDeclaration:
            mrv_ast_dump_r(ctx, self, as.CombinatorDeclaration.ident);
            mrv_ast_dump_r(ctx, self, as.CombinatorDeclaration.arg_list);
            mrv_ast_dump_r(ctx, self, as.CombinatorDeclaration.body);
            break;

        case MRV_ASTNodeKind_DeclarationArgList:
            for (uint32_t i = 0; i < as.DeclarationArgList.n_args; i++) {
                mrv_ast_dump_r(ctx, self, as.DeclarationArgList.args[i]);
            }
            break;

        case MRV_ASTNodeKind_DeclarationArg:
            mrv_ast_dump_r(ctx, self, as.DeclarationArg.ident);
            mrv_ast_dump_r(ctx, self, as.DeclarationArg.type);
            break;

        case MRV_ASTNodeKind_Block:
            lg_write(ctx->writer, lg_str8_lit("\n"));
            mrv_ast_dump_indent(ctx);
            lg_printf(ctx->writer, lg_str8_lit("subgraph \"cluster_%{i64}\" {\n"), self);
            ctx->indent++;
            for (uint32_t i = 0; i < as.Block.n_statements; i++) {
                mrv_ast_dump_r(ctx, self, as.Block.statements[i]);
            }
            ctx->indent--;
            mrv_ast_dump_indent(ctx);
            lg_write(ctx->writer, lg_str8_lit("}\n\n"));
            break;

        case MRV_ASTNodeKind_AssignmentStatement:
            mrv_ast_dump_r(ctx, self, as.AssignmentStatement.symbol_decl);
            mrv_ast_dump_r(ctx, self, as.AssignmentStatement.expression);
            break;

        case MRV_ASTNodeKind_ExpressionStatement:
            mrv_ast_dump_r(ctx, self, as.ExpressionStatement.expression);
            break;
        }

    mrv_ast_dump_indent(ctx);
    lg_printf(ctx->writer, lg_str8_lit("\"node_%{i64}\""), self);
    lg_str8 kind_str = mrv_ast_node_kind_as_str(self->kind);
    if (is_leaf) {
        lg_printf(ctx->writer, lg_str8_lit(" [label=\"%{str} (\\\"%{str}\\\")\"];\n"), kind_str, mrv_span_to_str8(self->span, ctx->text));
    } else {
        lg_printf(ctx->writer, lg_str8_lit(" [label=\"%{str}\"];\n"), kind_str);
    }

    if (parent != NULL) {
        mrv_ast_dump_indent(ctx);
        lg_printf(ctx->writer, lg_str8_lit("\"node_%{i64}\""), parent);
        lg_write(ctx->writer, lg_str8_lit(" -> "));
        lg_printf(ctx->writer, lg_str8_lit("\"node_%{i64}\""), self);
        lg_write(ctx->writer, lg_str8_lit(";\n"));
    }
}

void
mrv_ast_dump(MRV_AST *ast, LG_Writer *writer, lg_str8 text) {
    MRV_ASTDumpContext ctx = {
        .ast = ast,
        .writer = writer,
        .text = text,
    };

    lg_write(writer, lg_str8_lit(
        "digraph Abstract_Syntax_Tree {\n"
        // "     size=\"11,8.5!\";\n"
        // "     ratio=\"fill\";\n"
        // "     rankdir=TB;\n\n"
    ));
    ctx.indent++;
    mrv_ast_dump_r(&ctx, NULL, ast->root);
    ctx.indent--;
    lg_write(writer, lg_str8_lit("}\n"));
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// the meta-ir ir ir & some helpers
///
////////////////////////////////////////////////////////////////////////////////

typedef uint8_t
MRV_InstKind;
enum 
MRV_InstKind {
    MRV_InstKind_NOP,
    MRV_InstKind_Invocation,
    MRV_InstKind_Arg,
    MRV_InstKind_Lambda,
};

/// 0 represents no symbol, or an invalid symbol
typedef struct
MRV_Symbol {
    uint32_t id;
} MRV_Symbol;

typedef struct
MRV_InstRef {
    uint32_t idx;
} MRV_InstRef;

typedef struct
MRV_LanguageDescriptorRef {
    uint32_t idx;
} MRV_LanguageDescriptorRef;

typedef struct
MRV_Inst_Invocation {
    MRV_Symbol                 new_symbol;
    MRV_LanguageDescriptorRef  operator;
    MRV_Symbol                 left_arg;
    MRV_Symbol                 right_arg;
} MRV_Inst_Invocation;

typedef struct
MRV_Inst_Arg {
    MRV_Symbol sym;
} MRV_Inst_Arg;

typedef struct
MRV_Inst_Lambda {
    MRV_Symbol  new_symbol;
    uint32_t    args_len;
    uint32_t    body_len;
} MRV_Inst_Lambda;

typedef struct 
MRV_Inst {
    MRV_InstKind kind;
    union {
        MRV_Inst_Invocation  invocation;
        MRV_Inst_Arg         arg;
        MRV_Inst_Lambda      lambda;
    } as;
} MRV_Inst;

typedef struct 
MRV_SymbolTable {
    MRV_Span ident_span;
    MRV_LanguageDescriptorRef type;
    uint8_t scope_depth;
} MRV_SymbolTable;

typedef struct 
MRV_InstStream {
    uint32_t cap;
    uint32_t len;
    MRV_Inst *insts lg_check_bounds(cap);

    uint32_t symtab_cap;
    MRV_SymbolTable *symtab lg_check_bounds(symtab_cap);
} MRV_InstStream;

typedef struct
MRV_NameResolutionStackNode {
    lg_str8 str_ident;
    MRV_Symbol symbol;
    uint32_t scope_depth;
} MRV_NameResolutionStackNode;

typedef struct 
MRV_NameResolutionStack {
    uint32_t next_symbol_id;
    uint32_t max_height_cap;
    uint32_t current_height;
    MRV_NameResolutionStackNode *nodes lg_check_bounds(max_height_cap);
} MRV_NameResolutionStack;

#define mrv_match_inst(kind) switch ((enum MRV_InstKind)kind)

void
mrv_istream_init(
    MRV_InstStream *istream,
    LG_Arena *arena,
    uint32_t cap,
    uint32_t max_symbol_id
) {
    lg_memzero(istream, sizeof(MRV_InstStream));

    MRV_Inst *insts = lg_arena_alloc_array(arena, MRV_Inst, cap);
    lg_assert(insts != NULL);

    size_t symtab_cap = max_symbol_id + 1;
    MRV_SymbolTable *symtab = lg_arena_alloc_array(arena, MRV_SymbolTable, symtab_cap);
    lg_assert(symtab != NULL);

    istream->cap = cap;
    istream->symtab_cap = symtab_cap;
    istream->insts = insts;
    istream->symtab = symtab;
}

/// returns the index of the instruction
uint32_t
mrv_istream_append(
    MRV_InstStream *istream,
    MRV_Inst inst
) {
    lg_assert(istream->len + 1 < istream->cap);

    uint32_t idx = istream->len;

    istream->insts[idx] = inst;
    istream->len++;

    return idx;
}

void
mrv_nrstack_init(MRV_NameResolutionStack *nrstack, LG_Arena *arena, uint32_t max_height_cap) {
    MRV_NameResolutionStackNode *nodes = lg_arena_alloc_array(arena, MRV_NameResolutionStackNode, max_height_cap);
    lg_assert(nodes != NULL);

    lg_memzero(nrstack, sizeof(MRV_NameResolutionStack));
    nrstack->nodes = nodes;
    nrstack->max_height_cap = max_height_cap;
}

MRV_Symbol
mrv_nrstack_push(
    MRV_NameResolutionStack *nrstack,
    lg_str8 str_ident
) {
    lg_assert(nrstack != NULL);
    lg_assert(nrstack->current_height + 1 <= nrstack->max_height_cap);

    uint32_t depth = 0;
    if (nrstack->current_height > 0) {
        depth = nrstack->nodes[nrstack->current_height - 1].scope_depth;
    }

    // inc. first s.t 0 is an invalid symbol id
    nrstack->next_symbol_id++;

    MRV_Symbol sym = { .id = nrstack->next_symbol_id };
    nrstack->nodes[nrstack->current_height] = (MRV_NameResolutionStackNode){
        .scope_depth = depth,
        .str_ident = str_ident,
        .symbol = sym,
    };

    nrstack->current_height++;

    return sym;
}

MRV_Symbol
mrv_nrstack_find_name(
    MRV_NameResolutionStack *nrstack,
    lg_str8 name,
    bool *lg_nullable out_found
) {
    for (uint32_t depth = nrstack->current_height; depth > 0; depth--) {
        if (lg_strcmp(nrstack->nodes[depth - 1].str_ident, name) == 0) {
            if (out_found != NULL) {
                *out_found = true;
            }
            return nrstack->nodes[depth - 1].symbol;
        }
    }
    if (out_found != NULL) {
        *out_found = false;
    }
    return lg_nil(MRV_Symbol);
}

MRV_Symbol
mrv_nrstack_push_first_in_scope(
    MRV_NameResolutionStack *nrstack,
    lg_str8 str_ident
) {
    lg_assert(nrstack != NULL);
    MRV_Symbol sym = mrv_nrstack_push(nrstack, str_ident);
    nrstack->nodes[nrstack->current_height - 1].scope_depth++;
    return sym;
}

uint32_t
mrv_nrstack_get_scope_depth(MRV_NameResolutionStack *nrstack) {
    lg_assert(nrstack != NULL);
    return nrstack->current_height > 0 ?
        nrstack->nodes[nrstack->current_height - 1].scope_depth :
        0;
}

void
mrv_nrstack_pop_scope(MRV_NameResolutionStack *nrstack) {
    lg_assert(nrstack != NULL);

    uint32_t depth = 0;
    if (nrstack->current_height > 0) {
        depth = nrstack->nodes[nrstack->current_height - 1].scope_depth;
    }

    while (
        nrstack->nodes[nrstack->current_height - 1].scope_depth == depth &&
        nrstack->current_height > 0
    ) { nrstack->current_height--; }
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// semantic analysis stuff
///
////////////////////////////////////////////////////////////////////////////////

typedef uint8_t
MRV_TypeKind;
enum 
MRV_TypeKind {
    MRV_TypeKind_Nominal,
    MRV_TypeKind_Lambda,
    MRV_TypeKind_Host,
};

typedef uint8_t
MRV_LanguageDescriptorEntryKind;
enum {
    MRV_LanguageDescriptorEntryKind_Type,
    MRV_LanguageDescriptorEntryKind_Operator,
    MRV_LanguageDescriptorEntryKind_Combinator,
};

typedef struct
MRV_LanguageDescriptorEntry {
    MRV_LanguageDescriptorEntryKind kind;

    lg_str8 name;

    union {
        struct {
            MRV_TypeKind type_kind;

            /// the following are only active in the case that this is a lambda
            MRV_LanguageDescriptorRef return_type;
            MRV_LanguageDescriptorRef left_arg_type;
            MRV_LanguageDescriptorRef right_arg_type;
        } type;

        struct {
            lg_str8  left_arg_name;
            lg_str8  left_arg_type;
            lg_str8  right_arg_type;
            lg_str8  right_arg_name;
            lg_str8  return_type;
        } operator;

        struct {
            MRV_InstStream istream;
        } combinator;
    } as;
} MRV_LanguageDescriptorEntry;

typedef struct
MRV_LanguageDescriptor {
    LG_Arena                      arena;
    lg_str8                       language_name;
    LG_Table                      table;
    MRV_LanguageDescriptorEntry  *entries;
} MRV_LanguageDescriptor;

// miscellaneous state variables needed during some phases of recursive traversal,
// namely when moving the bodies of combinators from the AST into structured
// SSA
typedef struct 
MRV_SemaPhaseState {
    size_t           counting_n_insts;
    size_t           counting_max_symbol_id;
    MRV_InstStream  *istream;

    MRV_NameResolutionStack nrstack;
} MRV_SemaPhaseState;

typedef struct
MRV_SemaContext {
    MRV_AST                *ast;
    lg_str8                 text;
    MRV_LanguageDescriptor  ldesc;
    MRV_Error               err;
    LG_Arena               *scratch;
    MRV_SemaPhaseState      phase_state;
} MRV_SemaContext;

typedef void (*MRV_SemaVisitor)(MRV_SemaContext *ctx, MRV_ASTNode *self);

lg_force_inline void
mrv_sema_traverse_children(
    MRV_SemaContext *ctx,
    MRV_ASTNode *self,
    MRV_SemaVisitor next
) {
    MRV_ASTNodeChildren as = self->children_as;

    mrv_match_ast_node(self->kind) {
    case MRV_ASTNodeKind_Error:
    case MRV_ASTNodeKind_SymbolIdent:
    case MRV_ASTNodeKind_OtherIdent:
    case MRV_ASTNodeKind_Unit:
    case MRV_ASTNodeKind_HostTypeIdent:
        break;

    case MRV_ASTNodeKind_Program:
        for (uint32_t i = 0; i < as.Program.n_children; i++) {
            lg_assert(
                as.Program.children[i]->kind == MRV_ASTNodeKind_CombinatorDeclaration ||
                as.Program.children[i]->kind == MRV_ASTNodeKind_TypeDeclaration ||
                as.Program.children[i]->kind == MRV_ASTNodeKind_OperatorDeclaration ||
                as.Program.children[i]->kind == MRV_ASTNodeKind_LanguageDeclaration
            );
            next(ctx, as.Program.children[i]);
        }
        break;

    case MRV_ASTNodeKind_SymbolDeclaration: {
        lg_assert(as.SymbolDeclaration.symbol_ident->kind == MRV_ASTNodeKind_SymbolIdent);
        lg_assert(as.SymbolDeclaration.type_ident->kind == MRV_ASTNodeKind_OtherIdent);

        next(ctx, as.SymbolDeclaration.symbol_ident);
        next(ctx, as.SymbolDeclaration.type_ident);

        break;
    }

    case MRV_ASTNodeKind_InvocationArgList:
        for (size_t i = 0; i < as.InvocationArgList.n_args; i++) {
            lg_assert(
                as.InvocationArgList.args[i]->kind == MRV_ASTNodeKind_OtherIdent ||
                as.InvocationArgList.args[i]->kind == MRV_ASTNodeKind_Unit ||
                as.InvocationArgList.args[i]->kind == MRV_ASTNodeKind_SymbolIdent
            );
            next(ctx, as.InvocationArgList.args[i]);
        }
        break;

    case MRV_ASTNodeKind_LanguageDeclaration: {
        next(ctx, as.LanguageDeclaration.ident);
        break;
    }

    case MRV_ASTNodeKind_CombinatorDeclaration: {
        next(ctx, as.CombinatorDeclaration.ident);
        next(ctx, as.CombinatorDeclaration.arg_list);
        next(ctx, as.CombinatorDeclaration.body);
        break;
    }

    case MRV_ASTNodeKind_TypeDeclaration: {
        lg_assert(as.TypeDeclaration.ident->kind == MRV_ASTNodeKind_OtherIdent);
        lg_assert(
            as.TypeDeclaration.non_trivial_alias->kind == MRV_ASTNodeKind_NonTrivialType ||
            as.TypeDeclaration.non_trivial_alias->kind == MRV_ASTNodeKind_Error

        );
        next(ctx, as.TypeDeclaration.ident);
        next(ctx, as.TypeDeclaration.non_trivial_alias);
        break;
    }

    case MRV_ASTNodeKind_NonTrivialType: {
        lg_assert(as.NonTrivialType.outermost_ident->kind == MRV_ASTNodeKind_OtherIdent);
        lg_assert(as.NonTrivialType.invocation_arg_list->kind == MRV_ASTNodeKind_InvocationArgList);
        next(ctx, as.NonTrivialType.outermost_ident);
        next(ctx, as.NonTrivialType.invocation_arg_list);
        break;
    }

    case MRV_ASTNodeKind_OperatorDeclaration: {
        MRV_ASTNode *op_ident = as.OperatorDeclaration.ident;
        MRV_ASTNode *arg_list = as.OperatorDeclaration.arg_list;
        MRV_ASTNode *return_type = as.OperatorDeclaration.return_type;

        bool has_return = !mrv_ast_is_nil_node(ctx->ast, return_type);

        lg_assert(op_ident->kind == MRV_ASTNodeKind_OtherIdent);
        lg_assert(arg_list->kind == MRV_ASTNodeKind_DeclarationArgList);
        lg_assert(!has_return || return_type->kind == MRV_ASTNodeKind_OtherIdent);

        next(ctx, op_ident);
        next(ctx, arg_list);
        next(ctx, return_type);

        break;
    }

    case MRV_ASTNodeKind_DeclarationArg: {
        MRV_ASTNode *ident = as.DeclarationArg.ident;
        MRV_ASTNode *type = as.DeclarationArg.type;

        lg_assert(
            mrv_ast_is_nil_node(ctx->ast, ident) ||
            ident->kind == MRV_ASTNodeKind_OtherIdent ||
            ident->kind == MRV_ASTNodeKind_SymbolIdent
        );
        lg_assert(
            mrv_ast_is_nil_node(ctx->ast, type) ||
            type->kind == MRV_ASTNodeKind_OtherIdent ||
            type->kind == MRV_ASTNodeKind_HostTypeIdent
        );

        next(ctx, ident);
        next(ctx, type);

        break;
    }
        
    case MRV_ASTNodeKind_InvocationExpression: {
        MRV_ASTNode *ident = as.InvocationExpression.ident;
        MRV_ASTNode *arg_list = as.InvocationExpression.arg_list;

        lg_assert(ident->kind == MRV_ASTNodeKind_OtherIdent);
        lg_assert(arg_list->kind == MRV_ASTNodeKind_InvocationArgList);

        next(ctx, ident);
        next(ctx, arg_list);

        break;
    }

    case MRV_ASTNodeKind_LambdaExpression: {
        MRV_ASTNode *arg_list = as.LambdaExpression.decl_arg_list;
        MRV_ASTNode *body_block = as.LambdaExpression.body_block;

        lg_assert(arg_list->kind == MRV_ASTNodeKind_DeclarationArgList);
        lg_assert(body_block->kind == MRV_ASTNodeKind_Block);

        next(ctx, arg_list);
        next(ctx, body_block);

        break;
    }

    case MRV_ASTNodeKind_DeclarationArgList:
        for (uint32_t i = 0; i < as.DeclarationArgList.n_args; i++) {
            lg_assert(as.DeclarationArgList.args[i]->kind == MRV_ASTNodeKind_DeclarationArg);
            next(ctx, as.DeclarationArgList.args[i]);
        }
        break;
        
    case MRV_ASTNodeKind_Block:
        for (uint32_t i = 0; i < as.Block.n_statements; i++) {
            lg_assert(
                as.Block.statements[i]->kind == MRV_ASTNodeKind_AssignmentStatement ||
                as.Block.statements[i]->kind == MRV_ASTNodeKind_ExpressionStatement
            );
            next(ctx, as.Block.statements[i]);
        }
        break;

    case MRV_ASTNodeKind_AssignmentStatement:
        next(ctx, as.AssignmentStatement.symbol_decl);
        next(ctx, as.AssignmentStatement.expression);
        break;

    case MRV_ASTNodeKind_ExpressionStatement:
        next(ctx, as.ExpressionStatement.expression);
        break;
    }
}

void
mrv_sema_record_type_decls_r(MRV_SemaContext *ctx, MRV_ASTNode *self) {
    if (mrv_ast_is_nil_node(ctx->ast, self)) {
        return;
    }

    MRV_ASTNodeChildren as = self->children_as;

    mrv_match_ast_node(self->kind) {
    case MRV_ASTNodeKind_Program:
    case MRV_ASTNodeKind_OperatorDeclaration:
    case MRV_ASTNodeKind_InvocationExpression:
    case MRV_ASTNodeKind_CombinatorDeclaration: 
    case MRV_ASTNodeKind_DeclarationArg:
    case MRV_ASTNodeKind_DeclarationArgList: {
        mrv_sema_traverse_children(ctx, self, mrv_sema_record_type_decls_r);
        break;
    }

    case MRV_ASTNodeKind_HostTypeIdent: {
        lg_str8 ident = mrv_span_to_str8(self->span, ctx->text);

        size_t idx;
        LG_StatusKind status = lg_table_ensure_str8(
            &ctx->ldesc.table,
            ident,
            &idx,
            NULL
        );
        lg_assert(status == LG_StatusKind_OK);

        ctx->ldesc.entries[idx].name = ident;
        ctx->ldesc.entries[idx].kind = MRV_LanguageDescriptorEntryKind_Type;
        ctx->ldesc.entries[idx].as.type.type_kind = MRV_TypeKind_Host;

        break;
    }

    // we'll also take this opportunity to record the language name
    case MRV_ASTNodeKind_LanguageDeclaration: {
        lg_str8 language_name = mrv_span_to_str8(as.LanguageDeclaration.ident->span, ctx->text);
        if (
            ctx->ldesc.language_name.len != 0 && 
            (lg_strcmp(language_name, ctx->ldesc.language_name) != 0)
        ) {
            mrv_report_error(&ctx->err, self->span, lg_str8_lit("conflicting langauge declarations found"));
        }
        ctx->ldesc.language_name = language_name;
        break;
    }

    case MRV_ASTNodeKind_TypeDeclaration: {
        lg_str8 ident = mrv_span_to_str8(as.TypeDeclaration.ident->span, ctx->text);

        MRV_TypeKind type_kind;
        if (mrv_ast_is_nil_node(ctx->ast, as.TypeDeclaration.non_trivial_alias)) {
            type_kind = MRV_TypeKind_Nominal;
        } else {
            type_kind = MRV_TypeKind_Lambda;
        }

        size_t idx;
        bool found;
        LG_StatusKind status = lg_table_ensure_str8(
            &ctx->ldesc.table,
            ident,
            &idx,
            &found
        );
        if (found) {
            mrv_report_error(
                &ctx->err,
                self->span,
                lg_str8_lit("type %{str} declared multiple times"),
                ident
            );
            break;
        }
        lg_assert(status == LG_StatusKind_OK);

        if (type_kind == MRV_TypeKind_Lambda) {
            MRV_ASTNode *outer_ident = as.TypeDeclaration.non_trivial_alias->children_as.NonTrivialType.outermost_ident;
            MRV_ASTNode *arg_list = as.TypeDeclaration.non_trivial_alias->children_as.NonTrivialType.invocation_arg_list;

            size_t n_args = arg_list->children_as.InvocationArgList.n_args;
            lg_str8 outer_ident_str = mrv_span_to_str8(outer_ident->span, ctx->text);

            if (lg_strcmp(outer_ident_str, lg_str8_lit("Lambda")) != 0) {
                mrv_report_error(&ctx->err, outer_ident->span, lg_str8_lit(
                    "non-trivial type %{str} aliases a %{str} \n"
                    "non-trivial types must all be aliases to lambdas (for now)"
                ), ident, outer_ident_str);
                return;
            }

            if (n_args > 3) {
                mrv_report_error(&ctx->err, outer_ident->span, lg_str8_lit(
                    "type %{str}, a lambda, has more than three parameters\n"
                    "lambdas may only have three: (return type, left arg, right arg)"
                ), ident);
                return;
            }

            for (size_t i = 0; i < n_args; i++) {
                MRV_ASTNode *arg_node = arg_list->children_as.InvocationArgList.args[i];
                lg_str8 arg_str = mrv_span_to_str8(arg_node->span, ctx->text);

                MRV_LanguageDescriptorRef ldesc_ref;
                if (arg_node->kind == MRV_ASTNodeKind_Unit) {
                    ldesc_ref = (MRV_LanguageDescriptorRef){ .idx = 0 };
                } else if (arg_node->kind == MRV_ASTNodeKind_HostTypeIdent) {
                    mrv_report_error(&ctx->err, outer_ident->span, lg_str8_lit(
                        "%{str} is a host type, a parameter of the type %{str}, which is a lambda\n"
                        "lambdas cannot take or return host types"
                    ), arg_str, ident);
                    return;
                } else {
                    lg_assert(arg_node->kind == MRV_ASTNodeKind_OtherIdent);

                    bool found;
                    size_t arg_ldesc_idx = lg_table_get_str8(&ctx->ldesc.table, arg_str, &found);
                    if (!found) {
                        mrv_report_error(&ctx->err, outer_ident->span, lg_str8_lit(
                            "unknown type %{str} as parameter to type %{str}"
                            "lambdas cannot take or return host types"
                        ), arg_str, ident);
                        return;
                    }

                    ldesc_ref = (MRV_LanguageDescriptorRef){ .idx = arg_ldesc_idx };
                }

                if (i == 0) {
                    ctx->ldesc.entries[idx].as.type.return_type = ldesc_ref;
                } else if (i == 1) {
                    ctx->ldesc.entries[idx].as.type.left_arg_type = ldesc_ref;
                } else if (i == 2) {
                    ctx->ldesc.entries[idx].as.type.right_arg_type = ldesc_ref;
                } else {
                    lg_unreachable();
                }
            }
        }

        ctx->ldesc.entries[idx].name = ident;
        ctx->ldesc.entries[idx].kind = MRV_LanguageDescriptorEntryKind_Type;
        ctx->ldesc.entries[idx].as.type.type_kind = type_kind;

        break;
    }

    default:;
    }
}

void
mrv_sema_record_op_decls_r(MRV_SemaContext *ctx, MRV_ASTNode *self) {
    if (mrv_ast_is_nil_node(ctx->ast, self)) {
        return;
    }

    MRV_ASTNodeChildren as = self->children_as;

    mrv_match_ast_node(self->kind) {
    case MRV_ASTNodeKind_Program:
        mrv_sema_traverse_children(ctx, self, mrv_sema_record_op_decls_r);
        break;

    case MRV_ASTNodeKind_OperatorDeclaration: {
        MRV_ASTNode *op = as.OperatorDeclaration.ident;
        MRV_ASTNode *arg_list = as.OperatorDeclaration.arg_list;
        MRV_ASTNode *return_type = as.OperatorDeclaration.return_type;

        bool has_return = !mrv_ast_is_nil_node(ctx->ast, return_type);

        lg_str8 op_ident = mrv_span_to_str8(op->span, ctx->text);
        lg_str8 return_type_ident = has_return ?
            mrv_span_to_str8(return_type->span, ctx->text) :
            lg_nil(lg_str8);

        lg_str8 arg_names[2] = {0};
        lg_str8 arg_types[2] = {0};
        {
            size_t n_args = arg_list->children_as.DeclarationArgList.n_args;
            if (n_args > 2) {
                mrv_report_error(
                    &ctx->err,
                    arg_list->span,
                    lg_str8_lit(
                        "operator %{str} declared with %{i64} arguments\n"
                        "operators may not have more than two arguments"
                    ),
                    op_ident, n_args
                );
                break;
            }

            bool found;
            
            for (size_t i = 0; i < n_args; i++) {
                lg_assert(i < 2);
                MRV_Span name_ident_span = arg_list->children_as.DeclarationArgList.args[i]->children_as.DeclarationArg.ident->span;
                lg_str8 name_ident = mrv_span_to_str8(name_ident_span, ctx->text);
                MRV_Span type_ident_span = arg_list->children_as.DeclarationArgList.args[i]->children_as.DeclarationArg.type->span;
                lg_str8 type_ident = mrv_span_to_str8(type_ident_span, ctx->text);

                size_t idx = lg_table_get_str8(&ctx->ldesc.table, type_ident, &found);
                if (!found) {
                    mrv_report_error(
                        &ctx->err,
                        type_ident_span,
                        lg_str8_lit("unknown type in args of operator declaration: %{str}"),
                        type_ident
                    );
                    break;
                }
                if (ctx->ldesc.entries[idx].kind != MRV_LanguageDescriptorEntryKind_Type) {
                    mrv_report_error(
                        &ctx->err,
                        type_ident_span,
                        lg_str8_lit("type %{str} in args of operator declaration is not a type at all"),
                        type_ident
                    );
                    break;
                }

                arg_names[i] = name_ident;
                arg_types[i] = type_ident;
            }

            if (has_return) {
                size_t idx = lg_table_get_str8(&ctx->ldesc.table, return_type_ident, &found);
                if (!found) {
                    mrv_report_error(
                        &ctx->err,
                        return_type->span,
                        lg_str8_lit("unknown type in return type of operator declaration: %{str}"),
                        return_type_ident
                    );
                    break;
                }
                if (ctx->ldesc.entries[idx].kind != MRV_LanguageDescriptorEntryKind_Type) {
                    mrv_report_error(
                        &ctx->err,
                        return_type->span,
                        lg_str8_lit("type %{str} in args of operator declaration is not a type at all"),
                        return_type_ident
                    );
                    break;
                }
            }
        }

        size_t op_idx;
        {
            bool found;
            LG_StatusKind status = lg_table_ensure_str8(&ctx->ldesc.table, op_ident, &op_idx, &found);
            lg_assert(status == LG_StatusKind_OK);
            if (found) {
                mrv_report_error(
                    &ctx->err,
                    self->span,
                    lg_str8_lit("multiple declarations found for operator %{str}"),
                    op_ident
                );
                break;
            }
        }

        ctx->ldesc.entries[op_idx].name = op_ident;
        ctx->ldesc.entries[op_idx].kind = MRV_LanguageDescriptorEntryKind_Operator;
        ctx->ldesc.entries[op_idx].as.operator.left_arg_name = arg_names[0];
        ctx->ldesc.entries[op_idx].as.operator.left_arg_type = arg_types[0];
        ctx->ldesc.entries[op_idx].as.operator.right_arg_name = arg_names[1];
        ctx->ldesc.entries[op_idx].as.operator.right_arg_type = arg_types[1];
        ctx->ldesc.entries[op_idx].as.operator.return_type = return_type_ident;

        break;
    }

    default:;
    }
}

void
mrv_sema_combinator_do_counting(MRV_SemaContext *ctx, MRV_ASTNode *self) {
    MRV_SemaPhaseState *const state = &ctx->phase_state;

    mrv_match_ast_node(self->kind) {
    case MRV_ASTNodeKind_AssignmentStatement:
        state->counting_n_insts++;
        break;
    case MRV_ASTNodeKind_ExpressionStatement:
        state->counting_n_insts++;
        break;
    case MRV_ASTNodeKind_LambdaExpression:
        state->counting_n_insts++;
        break;
    case MRV_ASTNodeKind_DeclarationArg:
        state->counting_n_insts++;
        state->counting_max_symbol_id++;
        break;
    case MRV_ASTNodeKind_SymbolDeclaration:
        state->counting_max_symbol_id++;
        break;
    default:;
    }

    mrv_sema_traverse_children(ctx, self, mrv_sema_combinator_do_counting);
}

// forward decl. b/c mutual recursion
void
mrv_sema_block_to_inst_stream_r(MRV_SemaContext *ctx, MRV_ASTNode *self);

void
mrv_sema_append_inst_for_expr(MRV_SemaContext *ctx, MRV_ASTNode *self, MRV_Symbol new_symbol) {
    MRV_SemaPhaseState *const state = &ctx->phase_state;

    MRV_ASTNodeChildren as = self->children_as;

    mrv_match_ast_node(self->kind) {
    case MRV_ASTNodeKind_InvocationExpression: {
        lg_assert(as.InvocationExpression.ident->kind == MRV_ASTNodeKind_OtherIdent);
        lg_assert(as.InvocationExpression.arg_list->kind == MRV_ASTNodeKind_InvocationArgList);

        size_t n_args = as.InvocationExpression.arg_list->children_as.InvocationArgList.n_args;
        MRV_ASTNode *op_ident = as.InvocationExpression.ident;
        MRV_ASTNode *arg_list = as.InvocationExpression.arg_list;

        lg_assert(op_ident != NULL);
        lg_str8 op_ident_str = mrv_span_to_str8(op_ident->span, ctx->text);

        size_t operator_ldesc_idx;
        {
            bool found;
            operator_ldesc_idx = lg_table_get_str8(&ctx->ldesc.table, op_ident_str, &found);
            if (!found) {
                mrv_report_error(
                    &ctx->err,
                    op_ident->span,
                    lg_str8_lit("attempted to invoke unknown operator %{str}"),
                    op_ident_str
                );
                return;
            }
            if (ctx->ldesc.entries[operator_ldesc_idx].kind != MRV_LanguageDescriptorEntryKind_Operator) {
                mrv_report_error(
                    &ctx->err,
                    op_ident->span,
                    lg_str8_lit("attempted to invoke %{str}, which is not an operator"),
                    op_ident_str
                );
                return;
            }
        }

        if (n_args > 2) {
            mrv_report_error(
                &ctx->err,
                self->span,
                lg_str8_lit(
                    "operator %{str} passed more than two arguments\n"
                    "operators may have a maximum of two arguments"
                ), op_ident_str
            );
            return;
        }

        MRV_Symbol left_arg_symbol = {0};
        MRV_Span left_arg_span = {0};
        if (n_args > 0) {
            left_arg_span = arg_list->children_as.InvocationArgList.args[0]->span;
            lg_str8 name_str = mrv_span_to_str8(left_arg_span, ctx->text);

            bool found;
            left_arg_symbol = mrv_nrstack_find_name(&state->nrstack, name_str, &found);
            if (!found) {
                mrv_report_error(
                    &ctx->err,
                    left_arg_span,
                    lg_str8_lit("unknown identifier %{str} as first argument to invocation of operator %{str}"),
                    name_str, op_ident_str
                );
                return;
            }
        }

        MRV_Symbol right_arg_symbol = {0};
        MRV_Span right_arg_span = {0};
        if (n_args > 1) {
            right_arg_span = arg_list->children_as.InvocationArgList.args[1]->span;
            lg_str8 name_str = mrv_span_to_str8(right_arg_span, ctx->text);

            bool found;
            right_arg_symbol = mrv_nrstack_find_name(&state->nrstack, name_str, &found);
            if (!found) {
                mrv_report_error(
                    &ctx->err,
                    right_arg_span,
                    lg_str8_lit("unknown identifier %{str} as second argument to invocation of operator %{str}"),
                    name_str, op_ident_str
                );
                return;
            }
        }

        lg_assert(state->istream != NULL);
        mrv_istream_append(state->istream, (MRV_Inst){
            .kind = MRV_InstKind_Invocation,
            .as.invocation = {
                .new_symbol = new_symbol,
                .operator = { .idx = operator_ldesc_idx },
                .left_arg = left_arg_symbol,
                .right_arg = right_arg_symbol,
            },
        });

        break;
    }

    case MRV_ASTNodeKind_LambdaExpression: {
        MRV_ASTNode *arg_list = as.LambdaExpression.decl_arg_list;
        size_t n_args = arg_list->children_as.DeclarationArgList.n_args;

        uint32_t lambda_inst_idx = mrv_istream_append(state->istream, (MRV_Inst){
            .kind = MRV_InstKind_Lambda,
            .as.lambda = {
                .args_len = n_args,
                .body_len = 0, // backpatched
            },
        });

        for (uint32_t i = 0; i < n_args; i++) {
            MRV_ASTNode *arg = arg_list->children_as.DeclarationArgList.args[i];
            MRV_ASTNode *ident = arg->children_as.DeclarationArg.ident;
            MRV_ASTNode *type = arg->children_as.DeclarationArg.type;

            lg_str8 ident_str = mrv_span_to_str8(ident->span, ctx->text);
            lg_str8 type_str = mrv_span_to_str8(type->span, ctx->text);

            MRV_Symbol symbol;
            if (i == 0) {
                symbol = mrv_nrstack_push_first_in_scope(&state->nrstack, ident_str);
            } else {
                symbol = mrv_nrstack_push(&state->nrstack, ident_str);
            }

            MRV_LanguageDescriptorRef type_ref;
            {
                bool found;
                size_t type_ldesc_idx;
                LG_StatusKind status = lg_table_ensure_str8(&ctx->ldesc.table, type_str, &type_ldesc_idx, &found);
                lg_assert(status == LG_StatusKind_OK);
                if (!found) {
                    mrv_report_error(
                        &ctx->err,
                        type->span,
                        lg_str8_lit("unknown type %{str} found in assignment to lambda %{str}"),
                        type_str, ident_str 
                    );
                    return;
                }

                type_ref = (MRV_LanguageDescriptorRef){ .idx = type_ldesc_idx };
            }

            state->istream->symtab[symbol.id] = (MRV_SymbolTable){
                .ident_span = ident->span,
                .type = type_ref,
                .scope_depth = mrv_nrstack_get_scope_depth(&state->nrstack),
            };

            mrv_istream_append(state->istream, (MRV_Inst){
                .kind = MRV_InstKind_Arg,
                .as.arg = {
                    .sym = symbol,
                },
            });
        }

        mrv_sema_block_to_inst_stream_r(ctx, as.LambdaExpression.body_block);

        mrv_nrstack_pop_scope(&state->nrstack);

        uint32_t len = state->istream->len - lambda_inst_idx;
        state->istream->insts[lambda_inst_idx].as.lambda.body_len = len;

        break;
    }

    default:
        lg_unreachable();
        break;
    }
}

void
mrv_sema_block_to_inst_stream_r(MRV_SemaContext *ctx, MRV_ASTNode *self) {
    MRV_SemaPhaseState *const state = &ctx->phase_state;

    MRV_ASTNodeChildren as = self->children_as;

    mrv_match_ast_node(self->kind) {
    case MRV_ASTNodeKind_LambdaExpression: // handled above
        break;

    case MRV_ASTNodeKind_AssignmentStatement: {
        mrv_sema_traverse_children(ctx, self, mrv_sema_block_to_inst_stream_r);

        MRV_ASTNode *symbol_ident = as.AssignmentStatement.symbol_decl->children_as.SymbolDeclaration.symbol_ident;
        MRV_ASTNode *symbol_type_ident = as.AssignmentStatement.symbol_decl->children_as.SymbolDeclaration.type_ident;
        lg_str8 symbol_ident_str = mrv_span_to_str8(symbol_ident->span, ctx->text);
        lg_str8 symbol_type_ident_str = mrv_span_to_str8(symbol_type_ident->span, ctx->text);

        // we're not actually checking that the type is what it should be here, just that it exists at all so
        // we can get this reference
        MRV_LanguageDescriptorRef type_ref;
        {
            bool found;
            size_t type_ldesc_idx;
            LG_StatusKind status = lg_table_ensure_str8(&ctx->ldesc.table, symbol_type_ident_str, &type_ldesc_idx, &found);
            lg_assert(status == LG_StatusKind_OK);
            if (!found) {
                mrv_report_error(
                    &ctx->err,
                    symbol_type_ident->span,
                    lg_str8_lit("unknown type %{str} found in assignment to symbol %{str}"),
                    symbol_type_ident_str, symbol_ident_str
                );
                return;
            }

            type_ref = (MRV_LanguageDescriptorRef){ .idx = type_ldesc_idx };
        }

        MRV_Symbol new_symbol = {0};
        lg_assert(state->nrstack.nodes != NULL);
        new_symbol = mrv_nrstack_push(&state->nrstack, symbol_ident_str);

        state->istream->symtab[new_symbol.id].type = type_ref;
        mrv_sema_append_inst_for_expr(ctx, as.AssignmentStatement.expression, new_symbol);

        break;
    }

    default:
        mrv_sema_traverse_children(ctx, self, mrv_sema_block_to_inst_stream_r);
    }
}

void
mrv_sema_record_combinators(MRV_SemaContext *ctx, MRV_ASTNode *self) {
    if (self->kind != MRV_ASTNodeKind_CombinatorDeclaration) {
        mrv_sema_traverse_children(ctx, self, mrv_sema_record_combinators);
        return;
    }

    LG_Scope scope = lg_push_scope(ctx->scratch);

    LG_StatusKind status;
    MRV_ASTNodeChildren as = self->children_as;


    ////////////////////////////////////////////////////////////////////////
    // ~~ read a few things from the ast ~~ 

    MRV_Span ident_span = as.CombinatorDeclaration.ident->span;
    lg_str8 ident = mrv_span_to_str8(ident_span, ctx->text);

    size_t ldesc_idx;
    {
        bool found;
        status = lg_table_ensure_str8(&ctx->ldesc.table, ident, &ldesc_idx, &found);
        lg_assert(status == LG_StatusKind_OK);
        if (found) {
            mrv_report_error(
                &ctx->err,
                self->span,
                lg_str8_lit("found multiple declarations for combinator %{str}"),
                ident
            );
            goto out;
        }
    }

    MRV_LanguageDescriptorEntry *const entry = &ctx->ldesc.entries[ldesc_idx];

    size_t n_args = as.CombinatorDeclaration.arg_list->children_as.DeclarationArgList.n_args;
    MRV_ASTNode **arg_nodes = as.CombinatorDeclaration.arg_list->children_as.DeclarationArgList.args;

    if (n_args == 0) {
        mrv_report_error(
            &ctx->err,
            self->span,
            lg_str8_lit("combinator %{str} has no arguments\ncombinators must have at least one argument"),
            ident
        );
        goto out;
    }


    ////////////////////////////////////////////////////////////////////////
    // ~~ initialize the current phase state ~~ 

    MRV_SemaPhaseState *const state = &ctx->phase_state;

    lg_memzero(state, sizeof(MRV_SemaPhaseState));
    mrv_sema_combinator_do_counting(ctx, self);

    mrv_istream_init(
        &entry->as.combinator.istream,
        &ctx->ldesc.arena,
        state->counting_n_insts + n_args,
        state->counting_max_symbol_id
    );
    entry->kind = MRV_LanguageDescriptorEntryKind_Combinator;
    entry->name = ident;

    mrv_nrstack_init(&state->nrstack, ctx->scratch, state->counting_max_symbol_id);

    state->istream = &entry->as.combinator.istream;
    

    ////////////////////////////////////////////////////////////////////////
    // ~~ record args & traverse body to record everything else ~~ 

    // the istream of a combinator always begins with its args
    {
        mrv_istream_append(state->istream, (MRV_Inst){
            .kind = MRV_InstKind_Lambda,
            .as.lambda.args_len = n_args,
            .as.lambda.body_len = state->counting_n_insts,
        });
        for (size_t i = 0; i < n_args; i++) {
            MRV_Span arg_ident_span = arg_nodes[i]->children_as.DeclarationArg.ident->span;
            lg_str8 arg_ident = mrv_span_to_str8(arg_ident_span, ctx->text);

            MRV_Span arg_type_span = arg_nodes[i]->children_as.DeclarationArg.type->span;
            lg_str8 arg_type = mrv_span_to_str8(arg_type_span, ctx->text);
            
            bool found;

            mrv_nrstack_find_name(&state->nrstack, arg_ident, &found);
            if (found) {
                mrv_report_error(
                    &ctx->err,
                    arg_ident_span,
                    lg_str8_lit("multiple arguments found for combinator %{str} with identifier %{str}"),
                    ident, arg_ident
                );
                goto out;
            }

            MRV_LanguageDescriptorRef type_ref;
            {
                size_t ldesc_idx = lg_table_get_str8(&ctx->ldesc.table, arg_type, &found);
                if (!found) {
                    mrv_report_error(
                        &ctx->err,
                        self->span,
                        lg_str8_lit(
                            "argument %{str} in combinator %{str} has unknown type %{str}\n"
                        ),
                        arg_ident, ident, arg_type
                    );
                    goto out;
                }

                type_ref = (MRV_LanguageDescriptorRef){ .idx = ldesc_idx };
            }

            MRV_Symbol sym = mrv_nrstack_push(&state->nrstack, arg_ident);

            state->istream->symtab[sym.id] = (MRV_SymbolTable){
                .ident_span = arg_ident_span,
                .type = type_ref,
                .scope_depth = mrv_nrstack_get_scope_depth(&state->nrstack),
            };
            state->istream->symtab[sym.id].ident_span = arg_ident_span;
            state->istream->symtab[sym.id].type = (MRV_LanguageDescriptorRef){ .idx = ldesc_idx };
        }
    }

    lg_assert(as.CombinatorDeclaration.body->kind == MRV_ASTNodeKind_Block);

    if (as.CombinatorDeclaration.body->children_as.Block.n_statements == 0) {
        mrv_report_error(
            &ctx->err,
            self->span,
            lg_str8_lit(
                "found combinator %{str} with empty body\n"
                "combinators may not have empty bodies"
            ),
            ident
        );
        goto out;
    }

    mrv_sema_block_to_inst_stream_r(ctx, self);

out:
    lg_pop_scope(ctx->scratch, scope);
    return;
}

void
mrv_analyze(
    LG_Allocator *artifact_allocator,
    LG_Allocator *scratch_allocator,
    MRV_AST *ast,
    lg_str8 text,
    LG_Writer *err_writer,
    MRV_LanguageDescriptor *out_ldesc
) {
    lg_assert(out_ldesc != NULL);

    LG_Arena arena = {0};
    lg_arena_init(&arena, scratch_allocator);
    
    MRV_SemaContext ctx = {
        .ast = ast,
        .text = text,
        .scratch = &arena,
        .err.writer = err_writer,
    };


    //////////////////////////////////////////////
    /// ~~ initialize tables ~~
    // TODO: remove magic number capacity

    LG_StatusKind status = LG_StatusKind_OK;

    lg_arena_init(&ctx.ldesc.arena, artifact_allocator);
    status = lg_table_init(&ctx.ldesc.table, &ctx.ldesc.arena, 1024);
    lg_assert(status == LG_StatusKind_OK);
    ctx.ldesc.entries = lg_arena_alloc_array(&ctx.ldesc.arena, MRV_LanguageDescriptorEntry, 1024);
    lg_assert(ctx.ldesc.entries != NULL);


    //////////////////////////////////////////////
    /// ~~ do the type checking ~~

    mrv_sema_record_type_decls_r(&ctx, ctx.ast->root);
    mrv_sema_record_op_decls_r(&ctx, ctx.ast->root);
    mrv_sema_record_combinators(&ctx, ctx.ast->root);

    
    //////////////////////////////////////////////
    /// ~~ fin ~~
    
    *out_ldesc = ctx.ldesc;


    lg_arena_free_all(&arena);
}

void
mrv_ldesc_destroy(MRV_LanguageDescriptor *ldesc) {
    lg_arena_free_all(&ldesc->arena);
    lg_memzero(ldesc, sizeof(MRV_LanguageDescriptor));
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// text templates
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
MRV_TmplFieldTable {
    lg_str8 key;
    union {
        lg_str8 str;
        LG_StringList strlist;
    } value_as;
} MRV_TmplFieldTable;

void 
mrv_write_tmpl(
    LG_Writer *writer,
    lg_str8 text,
    MRV_TmplFieldTable *fields,
    size_t n_entries
) {
    for (size_t i = 0 ; i < text.len; i++) {
        if (
            text.p[i] == '$' &&
            (i + 1 < text.len && text.p[i + 1] == '{') &&
            (i + 2 < text.len && text.p[i + 2] == '{')
        ) {
            i += 2;

            size_t scan = i;
            while (
                (scan < text.len && text.p[scan] != '}') &&
                (scan + 1 < text.len && text.p[scan + 1] != '}')
            ) { scan++; }

            lg_str8 found_key = (lg_str8){ .len = scan - i, .p = text.p + i + 1 };

            if (
                found_key.len > 2 &&
                found_key.p[0] == 'L' &&
                found_key.p[1] == ':'
            ) {
                bool found = false;
                lg_str8 found_key_without_prefix = (lg_str8){ .len = found_key.len - 2, .p = found_key.p + 2 };
                LG_StringList found_value = {0};
                for (size_t i_table = 0; i_table < n_entries; i_table++) {
                    if (lg_strcmp(found_key_without_prefix, fields[i_table].key) == 0) {
                        found = true;
                        found_value = fields[i_table].value_as.strlist;
                        break;
                    }
                }

                lg_assert(found);
                lg_strlist_write(&found_value, writer);
            } else {
                bool found = false;
                lg_str8 found_value = {0};
                for (size_t i_table = 0; i_table < n_entries; i_table++) {
                    if (lg_strcmp(found_key, fields[i_table].key) == 0) {
                        found = true;
                        found_value = fields[i_table].value_as.str;
                        break;
                    }
                }

                lg_assert(found);
                lg_write(writer, found_value);
            }

            i = scan + 2;
        } else {
            lg_write(writer, ((lg_str8){ .len = 1, .p = text.p + i }));
        }
    }
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// the sourcegen stuff
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
MRV_SourcegenContext {
    LG_Arena                *scratch;
    LG_Writer               *header_file_writer;
    LG_Writer               *source_file_writer;
    MRV_LanguageDescriptor  *ldesc;
    lg_str8                  type_ident_prefix;
    lg_str8                  text;

    struct {
        lg_str8 lang_capitalized;
        lg_str8 lang_snake_case;
    } common_strings;
} MRV_SourcegenContext;

lg_str8
mrv_sg_fmt_symbol_type(MRV_SourcegenContext *ctx, lg_str8 name) {
    LG_StatusKind status = LG_StatusKind_OK;

    bool found;
    size_t idx = lg_table_get_str8(&ctx->ldesc->table, name, &found);
    lg_assert(found);

    MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];

    lg_assert(entry.kind == MRV_LanguageDescriptorEntryKind_Type);

    lg_str8 cat = {0};
    if (
        entry.as.type.type_kind == MRV_TypeKind_Nominal || 
        entry.as.type.type_kind == MRV_TypeKind_Lambda
    ) {
        status = lg_strcat(ctx->scratch, (lg_str8[]){
            lg_str8_lit("LG_"),
            ctx->ldesc->language_name,
            lg_str8_lit("Symbol_"),
            entry.name,
        }, 4, &cat);
        lg_assert(status == LG_StatusKind_OK);
    } else if (entry.as.type.type_kind == MRV_TypeKind_Host) {
        status = lg_strcat(ctx->scratch, (lg_str8[]){
            entry.name,
            lg_str8_lit(" *"),
        }, 2, &cat);
        lg_assert(status == LG_StatusKind_OK);
    } else {
        lg_unreachable();
    }

    return cat;
}

#define MRV_DEF_C_KEYWORDS \
    MRV_X(char) \
    MRV_X(int) \
    MRV_X(float) \
    MRV_X(double) \
    MRV_X(short) \
    MRV_X(long) \
    MRV_X(signed) \
    MRV_X(unsigned) \
    MRV_X(void) \
    MRV_X(if) \
    MRV_X(else) \
    MRV_X(switch) \
    MRV_X(case) \
    MRV_X(default) \
    MRV_X(for) \
    MRV_X(while) \
    MRV_X(do) \
    MRV_X(break) \
    MRV_X(continue) \
    MRV_X(goto) \
    MRV_X(return) \
    MRV_X(auto) \
    MRV_X(register) \
    MRV_X(static) \
    MRV_X(extern) \
    MRV_X(struct) \
    MRV_X(union) \
    MRV_X(enum) \
    MRV_X(typedef) \
    MRV_X(const) \
    MRV_X(volatile) \
    MRV_X(sizeof)

const struct {
    lg_str8 str;
    uint32_t hash;
}
MRV_C_KEYWORDS[] = {
#   define MRV_X(kw) { .str = lg_str8_lit(#kw), .hash = lg_hash_lit_16(#kw) },
    MRV_DEF_C_KEYWORDS
#   undef MRV_X
};
const uint32_t 
MRV_N_C_KEYWORDS = sizeof(MRV_C_KEYWORDS) / sizeof(MRV_C_KEYWORDS[0]);

/// allocates a "safe" version of some (currently pascal case) identifier that follows two rules:
/// 1) it is snake case
/// 2) avoids C language keywords
lg_str8
mrv_sg_pascal_to_snake_escaped(LG_Arena *arena, lg_str8 original) {
    lg_assert(original.len != 0);

    LG_StatusKind status;

    lg_str8 name_snake_case;
    status = lg_str8_pascal_to_snake_case(original, arena, &name_snake_case);
    lg_assert(status == LG_StatusKind_OK);

    if (name_snake_case.len > 16) {
        return name_snake_case;
    }

    uint64_t this_hash = lg_hash_16(name_snake_case.p, name_snake_case.len);
    for (uint32_t i = 0; i < MRV_N_C_KEYWORDS; i++) {
        uint64_t reserved_hash = MRV_C_KEYWORDS[i].hash;
        if (
            this_hash != reserved_hash ||
            lg_strcmp(MRV_C_KEYWORDS[i].str, name_snake_case) != 0
        ) {
            continue;
        }

        // waste memory who cares
        lg_str8 cat;
        status = lg_strcat(arena, (lg_str8[]){name_snake_case, lg_str8_lit("_")}, 2, &cat);
        lg_assert(status == LG_StatusKind_OK);

        return cat;
    }

    return name_snake_case;
}

void
mrv_sg_type_enum(MRV_SourcegenContext *ctx) {
    lg_printf(ctx->header_file_writer, lg_str8_lit(
        "\ntypedef uint8_t\nLG_%{str}Type;\n"
        "enum\nLG_%{str}Type {\n"
    ), ctx->ldesc->language_name, ctx->ldesc->language_name);

    LG_TableIter iter = {0};
    lg_table_iter_init(&iter, &ctx->ldesc->table);

    size_t idx;
    while (lg_table_iter_advance(&iter, &idx, NULL)) {
        MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];
        if (
            entry.kind != MRV_LanguageDescriptorEntryKind_Type ||
            entry.as.type.type_kind == MRV_TypeKind_Host
        ) {
            continue;
        }

        lg_printf(
            ctx->header_file_writer,
            lg_str8_lit("    LG_%{str}Type_%{str},\n"),
            ctx->ldesc->language_name, entry.name
        );
    }

    lg_write(ctx->header_file_writer, lg_str8_lit("};\n"));
}

void
mrv_sg_opcode_enum(MRV_SourcegenContext *ctx) {
    lg_printf(ctx->header_file_writer, lg_str8_lit(
        "\ntypedef uint8_t\nLG_%{str}Opcode;\n"
        "enum\nLG_%{str}Opcode {\n"
    ), ctx->ldesc->language_name, ctx->ldesc->language_name);

    LG_TableIter iter = {0};
    lg_table_iter_init(&iter, &ctx->ldesc->table);

    size_t idx;
    while (lg_table_iter_advance(&iter, &idx, NULL)) {
        MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];
        if (entry.kind == MRV_LanguageDescriptorEntryKind_Operator) {
            lg_printf(
                ctx->header_file_writer,
                lg_str8_lit("    LG_%{str}Opcode_%{str},\n"),
                ctx->ldesc->language_name, entry.name
            );
        }
    }

    lg_write(ctx->header_file_writer, lg_str8_lit("};\n"));
}

void
mrv_sg_symbol_types(MRV_SourcegenContext *ctx) {
    LG_TableIter iter = {0};
    lg_table_iter_init(&iter, &ctx->ldesc->table);

    size_t idx;
    while (lg_table_iter_advance(&iter, &idx, NULL)) {
        MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];
        if (
            entry.kind != MRV_LanguageDescriptorEntryKind_Type ||
            entry.as.type.type_kind == MRV_TypeKind_Host
        ) {
            continue;
        }

        lg_write(ctx->header_file_writer, lg_str8_lit("\ntypedef struct\n"));
        lg_write(ctx->header_file_writer, mrv_sg_fmt_symbol_type(ctx, entry.name));
        lg_write(ctx->header_file_writer, lg_str8_lit(" {\n    uint32_t id;"));

        if (entry.as.type.type_kind == MRV_TypeKind_Lambda) {
            lg_write(ctx->header_file_writer, lg_str8_lit(
                "\n    uint32_t args_len;"
                "\n    uint32_t body_len;"
            ));
        }

        lg_write(ctx->header_file_writer, lg_str8_lit("\n} "));
        lg_write(ctx->header_file_writer, mrv_sg_fmt_symbol_type(ctx, entry.name));
        lg_write(ctx->header_file_writer, lg_str8_lit(";\n"));
    }
}

void
mrv_sg_node_types(MRV_SourcegenContext *ctx) {
    LG_TableIter iter = {0};
    lg_table_iter_init(&iter, &ctx->ldesc->table);

    size_t idx;
    while (lg_table_iter_advance(&iter, &idx, NULL)) {
        MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];
        if (entry.kind == MRV_LanguageDescriptorEntryKind_Operator) {
            lg_printf(
                ctx->header_file_writer,
                lg_str8_lit("\ntypedef struct\nLG_%{str}Node_%{str} {"),
                ctx->ldesc->language_name, entry.name
            );

            if (entry.as.operator.left_arg_name.len != 0) {
                lg_write(ctx->header_file_writer, lg_str8_lit("\n    "));
                lg_write(ctx->header_file_writer, mrv_sg_fmt_symbol_type(ctx, entry.as.operator.left_arg_type));
                lg_printf(
                    ctx->header_file_writer,
                    lg_str8_lit(" %{str};"),
                    entry.as.operator.left_arg_name
                );
            }
            if (entry.as.operator.right_arg_name.len != 0) {
                lg_write(ctx->header_file_writer, lg_str8_lit("\n    "));
                lg_write(ctx->header_file_writer, mrv_sg_fmt_symbol_type(ctx, entry.as.operator.right_arg_type));
                lg_printf(
                    ctx->header_file_writer,
                    lg_str8_lit(" %{str};"),
                    entry.as.operator.right_arg_name
                );
            }
            if (entry.as.operator.return_type.len != 0) {
                lg_write(ctx->header_file_writer, lg_str8_lit("\n    "));
                lg_write(ctx->header_file_writer, mrv_sg_fmt_symbol_type(ctx, entry.as.operator.return_type));
                lg_write(ctx->header_file_writer, lg_str8_lit(" return_val;"));
            }
            
            lg_printf(
                ctx->header_file_writer,
                lg_str8_lit("\n} LG_%{str}Node_%{str};\n"),
                ctx->ldesc->language_name, entry.name
            );
        }
    }
}

void
mrv_sg_node_union_type(MRV_SourcegenContext *ctx) {
    lg_printf(ctx->header_file_writer, lg_str8_lit("\ntypedef union\nLG_%{str}Operands {"), ctx->ldesc->language_name);
    {
        LG_TableIter iter = {0};
        lg_table_iter_init(&iter, &ctx->ldesc->table);

        size_t idx;
        while (lg_table_iter_advance(&iter, &idx, NULL)) {
            LG_Scope scope = lg_push_scope(ctx->scratch);

            MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];
            lg_str8 name_snake_case = mrv_sg_pascal_to_snake_escaped(ctx->scratch, entry.name);

            if (entry.kind == MRV_LanguageDescriptorEntryKind_Operator) {
                lg_printf(ctx->header_file_writer, lg_str8_lit("\n    LG_%{str}Node_%{str} %{str};"), ctx->ldesc->language_name, entry.name, name_snake_case);
            }

            lg_pop_scope(ctx->scratch, scope);
        }
    }
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n} LG_%{str}Operands;\n"), ctx->ldesc->language_name);

    lg_printf(ctx->header_file_writer, lg_str8_lit("\ntypedef struct\nLG_%{str}Node {"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n    LG_%{str}Opcode opcode;"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n    LG_%{str}Operands as;"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n} LG_%{str}Node;\n"), ctx->ldesc->language_name);
}

void
mrv_sg_builder_types(MRV_SourcegenContext *ctx) {
    lg_printf(ctx->header_file_writer, lg_str8_lit("\ntypedef struct\nLG_%{str}NodeList {"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n    struct LG_%{str}NodeList *prev;"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n    LG_%{str}Node node;"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n} LG_%{str}NodeList;\n"), ctx->ldesc->language_name);

    lg_printf(ctx->header_file_writer, lg_str8_lit("\ntypedef struct\nLG_%{str}Builder {"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n    LG_%{str}NodeList *nodes_tail;"), ctx->ldesc->language_name);
    lg_write(ctx->header_file_writer, lg_str8_lit("\n    uint32_t next_symbol_id;"));
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n} LG_%{str}Builder;\n"), ctx->ldesc->language_name);
}

void
mrv_sg_expr_type(MRV_SourcegenContext *ctx) {
    lg_printf(ctx->header_file_writer, lg_str8_lit("\ntypedef struct\nLG_%{str}Expr {"), ctx->ldesc->language_name);
    lg_write(ctx->header_file_writer, lg_str8_lit("\n    size_t cap;"));
    lg_write(ctx->header_file_writer, lg_str8_lit("\n    size_t len;"));
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n    LG_%{str}Node *nodes;"), ctx->ldesc->language_name);
    lg_printf(ctx->header_file_writer, lg_str8_lit("\n} LG_%{str}Expr;\n"), ctx->ldesc->language_name);
}

void
mrv_sg_redex_types(MRV_SourcegenContext *ctx) {
    const lg_str8 template = lg_str8_lit(R"(
typedef struct
LG_${{lang_name}}Redex_${{comb_name}} {${{L:members}}
} LG_${{lang_name}}Redex_${{comb_name}};
)");

    LG_TableIter iter = {0};
    lg_table_iter_init(&iter, &ctx->ldesc->table);

    size_t idx;
    while (lg_table_iter_advance(&iter, &idx, NULL)) {
        MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];

        if (entry.kind != MRV_LanguageDescriptorEntryKind_Combinator) {
            continue;
        }

        LG_StringList members = {0};
        // the first n arg nodes of the instruction stream are the args of the combinator itself
        uint32_t i_current_inst = 0;
        while (
            entry.as.combinator.istream.insts[i_current_inst].kind != MRV_InstKind_Arg &&
            i_current_inst < entry.as.combinator.istream.len
        ) { i_current_inst++; }

        while (
            entry.as.combinator.istream.insts[i_current_inst].kind == MRV_InstKind_Arg &&
            i_current_inst < entry.as.combinator.istream.len
        ) {
            MRV_Inst_Arg arg = entry.as.combinator.istream.insts[i_current_inst].as.arg;
            MRV_SymbolTable symtab_entry = entry.as.combinator.istream.symtab[arg.sym.id];
            MRV_LanguageDescriptorEntry type_entry = ctx->ldesc->entries[symtab_entry.type.idx];

            lg_str8 ident_str = mrv_span_to_str8(symtab_entry.ident_span, ctx->text);

            lg_strlist_append(&members, ctx->scratch, lg_str8_lit("\n    "));
            lg_strlist_append(&members, ctx->scratch, mrv_sg_fmt_symbol_type(ctx, type_entry.name));
            lg_strlist_append(&members, ctx->scratch, lg_str8_lit(" "));
            lg_strlist_append(&members, ctx->scratch, ident_str);
            lg_strlist_append(&members, ctx->scratch, lg_str8_lit(";"));

            i_current_inst++;
        }

        MRV_TmplFieldTable fields[] = {
            {lg_str8_lit("lang_name"),  { .str = ctx->ldesc->language_name }},
            {lg_str8_lit("comb_name"),  { .str = entry.name }},
            {lg_str8_lit("members"),    { .strlist = members }},
        };
        mrv_write_tmpl(ctx->header_file_writer, template, fields, sizeof(fields) / sizeof(MRV_TmplFieldTable));
    }
}

void
mrv_sg_append_fn(MRV_SourcegenContext *ctx) {
    const lg_str8 header_tmpl = lg_str8_lit(R"(
lg_force_inline ${{L:return_type}}
lg_hbuilder_${{op_snake}}(
    LG_Context *ctx,
    LG_${{lang_name}}Builder *builder${{L:operands}}
);
)");
    const lg_str8 source_tmpl = lg_str8_lit(R"(
lg_force_inline ${{L:return_type}}
lg_hbuilder_${{op_snake}}(
    LG_Context *ctx,
    LG_${{lang_name}}Builder *builder${{L:operands}}
) {
    LG_${{lang_name}}NodeList *node = lg_arena_alloc_struct(&ctx->arena, LG_${{lang_name}}NodeList);
    if (node == NULL) {
        lg_report_error(ctx, LG_StatusKind_OutOfMemory, lg_str8_lit("ran out of memory appending to ${{lang_snake}} expr"));
        ${{L:early_return_statement}}
    }

    // increment first so zero is not a valid symbol id
    builder->next_symbol_id++;
    
    node->node.opcode = LG_${{lang_name}}Opcode_${{op}};
    node->node.as.${{op_var_ident}} = (LG_${{lang_name}}Node_${{op}}){${{L:props}}};

    if (builder->nodes_tail != NULL) {
        node->prev = builder->nodes_tail;
    }
    builder->nodes_tail = node;)");
    
    LG_TableIter iter = {0};
    lg_table_iter_init(&iter, &ctx->ldesc->table);

    size_t idx;
    while (lg_table_iter_advance(&iter, &idx, NULL)) {
        MRV_LanguageDescriptorEntry entry = ctx->ldesc->entries[idx];

        if (entry.kind != MRV_LanguageDescriptorEntryKind_Operator) {
            continue;
        }

        LG_Scope scope = lg_push_scope(ctx->scratch);
        LG_StatusKind status = LG_StatusKind_OK;
        lg_str8 var_ident = mrv_sg_pascal_to_snake_escaped(ctx->scratch, entry.name);
        lg_str8 name_snake;
        status = lg_str8_pascal_to_snake_case(entry.name, ctx->scratch, &name_snake);
        lg_assert(status == LG_StatusKind_OK);

        LG_StringList operands = {0};
        LG_StringList props = {0};

        if (entry.as.operator.left_arg_name.len > 0) {
            lg_str8 arg_name_snake = {0};
            status = lg_str8_pascal_to_snake_case(entry.as.operator.left_arg_name, ctx->scratch, &arg_name_snake);
            lg_assert(status == LG_StatusKind_OK);

            bool found;
            size_t arg_idx = lg_table_get_str8(&ctx->ldesc->table, entry.as.operator.left_arg_type, &found);
            lg_assert(found);
            lg_assert(ctx->ldesc->entries[arg_idx].kind == MRV_LanguageDescriptorEntryKind_Type);

            lg_strlist_append(&operands, ctx->scratch, lg_str8_lit(",\n    "));
            lg_strlist_append(&operands, ctx->scratch, mrv_sg_fmt_symbol_type(ctx, entry.as.operator.left_arg_type));
            lg_strlist_append(&operands, ctx->scratch, lg_str8_lit(" "));
            lg_strlist_append(&operands, ctx->scratch, arg_name_snake);

            lg_strlist_append(&props, ctx->scratch, lg_str8_lit("\n        ."));
            lg_strlist_append(&props, ctx->scratch, arg_name_snake);
            lg_strlist_append(&props, ctx->scratch, lg_str8_lit(" = "));
            lg_strlist_append(&props, ctx->scratch, arg_name_snake);
            lg_strlist_append(&props, ctx->scratch, lg_str8_lit(","));
        }
        if (entry.as.operator.right_arg_name.len > 0) {
            lg_str8 arg_name_snake = {0};
            status = lg_str8_pascal_to_snake_case(entry.as.operator.right_arg_name, ctx->scratch, &arg_name_snake);
            lg_assert(status == LG_StatusKind_OK);

            bool found;
            size_t arg_idx = lg_table_get_str8(&ctx->ldesc->table, entry.as.operator.right_arg_type, &found);
            lg_assert(found);
            lg_assert(ctx->ldesc->entries[arg_idx].kind == MRV_LanguageDescriptorEntryKind_Type);

            lg_strlist_append(&operands, ctx->scratch, lg_str8_lit(",\n    "));
            lg_strlist_append(&operands, ctx->scratch, mrv_sg_fmt_symbol_type(ctx, entry.as.operator.right_arg_type));
            lg_strlist_append(&operands, ctx->scratch, lg_str8_lit(" "));
            lg_strlist_append(&operands, ctx->scratch, arg_name_snake);

            lg_strlist_append(&props, ctx->scratch, lg_str8_lit("\n        ."));
            lg_strlist_append(&props, ctx->scratch, arg_name_snake);
            lg_strlist_append(&props, ctx->scratch, lg_str8_lit(" = "));
            lg_strlist_append(&props, ctx->scratch, arg_name_snake);
            lg_strlist_append(&props, ctx->scratch, lg_str8_lit(","));
        }

        LG_StringList early_return_statement = {0};
        LG_StringList return_type = {0};
        if (entry.as.operator.return_type.len > 0) {
            lg_strlist_append(&return_type, ctx->scratch, lg_str8_lit("LG_"));
            lg_strlist_append(&return_type, ctx->scratch, ctx->ldesc->language_name);
            lg_strlist_append(&return_type, ctx->scratch, lg_str8_lit("Symbol_"));
            lg_strlist_append(&return_type, ctx->scratch, entry.as.operator.return_type);

            lg_strlist_append(&props, ctx->scratch, lg_str8_lit("\n        .return_val = "));
            lg_strlist_append(&props, ctx->scratch, lg_str8_lit("{ .id = builder->next_symbol_id },"));

            lg_strlist_append(&early_return_statement, ctx->scratch, lg_str8_lit("return lg_nil("));
            lg_strlist_append(&early_return_statement, ctx->scratch, lg_str8_lit("LG_"));
            lg_strlist_append(&early_return_statement, ctx->scratch, ctx->ldesc->language_name);
            lg_strlist_append(&early_return_statement, ctx->scratch, lg_str8_lit("Symbol_"));
            lg_strlist_append(&early_return_statement, ctx->scratch, entry.as.operator.return_type);
            lg_strlist_append(&early_return_statement, ctx->scratch, lg_str8_lit(");"));
        } else {
            lg_strlist_append(&return_type, ctx->scratch, lg_str8_lit("void"));
            lg_strlist_append(&early_return_statement, ctx->scratch, lg_str8_lit("return;"));
        }

        if (props.tail != NULL) {
            lg_strlist_append(&props, ctx->scratch, lg_str8_lit("\n    "));
        }

        MRV_TmplFieldTable fields[] = {
            {lg_str8_lit("lang_name"),               { .str = ctx->ldesc->language_name }},
            {lg_str8_lit("lang_snake"),              { .str = ctx->common_strings.lang_snake_case}},
            {lg_str8_lit("return_type"),             { .strlist = return_type }},
            {lg_str8_lit("early_return_statement"),  { .strlist = early_return_statement }},
            {lg_str8_lit("op"),                      { .str = entry.name }},
            {lg_str8_lit("op_snake"),                { .str = name_snake }},
            {lg_str8_lit("op_var_ident"),            { .str = var_ident }},
            {lg_str8_lit("operands"),                { .strlist = operands }},
            {lg_str8_lit("props"),                   { .strlist = props }},
        };
        mrv_write_tmpl(ctx->header_file_writer, header_tmpl, fields, sizeof(fields) / sizeof(MRV_TmplFieldTable));
        mrv_write_tmpl(ctx->source_file_writer, source_tmpl, fields, sizeof(fields) / sizeof(MRV_TmplFieldTable));

        if (entry.as.operator.return_type.len > 0) {
            lg_printf(ctx->source_file_writer, lg_str8_lit(
                "\n\n    return (LG_%{str}Symbol_%{str}){ .id = builder->next_symbol_id };"
            ), ctx->ldesc->language_name, entry.as.operator.return_type);
        }

        lg_write(ctx->source_file_writer, lg_str8_lit("\n}\n"));

        lg_pop_scope(ctx->scratch, scope);
    }
}

void
mrv_gen_source(
    LG_Writer *header_file_writer,
    LG_Writer *source_file_writer,
    LG_Arena *scratch_allocator,
    MRV_LanguageDescriptor *ldesc,
    lg_str8 text
) {
    MRV_SourcegenContext ctx = {
        .header_file_writer = header_file_writer,
        .source_file_writer = source_file_writer,
        .text = text,
        .scratch = scratch_allocator,
        .ldesc = ldesc,
    };

    LG_Scope scope = lg_push_scope(ctx.scratch);

    // common strigs
    {
        LG_StatusKind status = LG_StatusKind_OK;

        status = lg_str8_to_upper(ldesc->language_name, ctx.scratch, &ctx.common_strings.lang_capitalized);
        status = lg_str8_pascal_to_snake_case(ldesc->language_name, ctx.scratch, &ctx.common_strings.lang_snake_case);

        lg_assert(status == LG_StatusKind_OK);
    }

    lg_printf(
        header_file_writer, 
        lg_str8_lit(
            "#ifndef LG_%{str}_GEN_H_\n"
            "#define LG_%{str}_GEN_H_\n"
        ), 
        ctx.common_strings.lang_capitalized,
        ctx.common_strings.lang_capitalized
    );
    lg_write(header_file_writer, lg_str8_lit("\n#include <libgrad/internal/base.h>\n"));
    {
        mrv_sg_type_enum(&ctx);
        mrv_sg_opcode_enum(&ctx);
        mrv_sg_symbol_types(&ctx);
        mrv_sg_node_types(&ctx);
        mrv_sg_node_union_type(&ctx);
        mrv_sg_builder_types(&ctx);
        mrv_sg_expr_type(&ctx);
        mrv_sg_redex_types(&ctx);
        mrv_sg_append_fn(&ctx);
    }
    lg_printf(header_file_writer, lg_str8_lit("\n#endif // LG_%{str}_GEN_H_\n"), ctx.common_strings.lang_capitalized);

    lg_pop_scope(ctx.scratch, scope);
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// actual entry point
///
////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>

void*
alloc_libc(void *_, size_t bytes) {
    (void)_;
    return calloc(bytes, 1);
}

void 
free_libc(void* _, void *ptr) {
    (void)_;
    return free(ptr);
}

size_t
write_stdout(void *ctx, lg_str8 msg) {
    (void)ctx;
    return printf("%.*s", (int32_t)msg.len, msg.p);
}

static LG_Allocator 
libc_allocator = {
    .alloc = alloc_libc,
    .free = free_libc,
    .default_slab_size_bytes = 1024 * 1024 * 1024,
};

static LG_Writer 
libc_writer = {
    .write = write_stdout,
};

int 
main(int32_t argc, char **argv) {
    int32_t ret_code = 0;

    LG_Arena scratch_allocator = {0};
    lg_arena_init(&scratch_allocator, &libc_allocator);
    
    lg_str8 *args = lg_arena_alloc_array(&scratch_allocator, lg_str8, argc);
    lg_assert(args != NULL);
    for (int32_t i = 0; i < argc; i++) {
        args[i] = lg_str8_from_cstr((uint8_t*)argv[i]);
    }

    if (argc < 2) {
        lg_printf(&libc_writer, lg_str8_lit("provide the input file as the first argument\n"));
        ret_code = -1;
        goto out_free_all;
    }

    // lg_assert(args[0].p[args[0].len] == 0);
    FILE *file = fopen((const char*)args[1].p, "r+");
    lg_assert(file != NULL);

    uint8_t file_contents[4096] = {0};
    size_t chunks_read = fread(file_contents, sizeof(file_contents) / 4, 4, file);
    lg_assert(chunks_read > 0);

    lg_str8 text = (lg_str8){ .len = 4096, .p = file_contents };
    MRV_TokenStream tstream = mrv_lex(&libc_allocator, text, &libc_writer);

    (void)text;

    MRV_AST ast = mrv_parse(
        &libc_allocator,
        &libc_allocator,
        &libc_writer,
        &tstream,
        text
    );

    for (int32_t i = 0; i < argc; i++) {
        if (lg_strcmp(args[i], lg_str8_lit("--dump-ast")) == 0) {
            mrv_ast_dump(&ast, &libc_writer, text);
            ret_code = 0;
            goto out_destroy_ast;
        }
    }

    MRV_LanguageDescriptor ldesc = {0};
    mrv_analyze(
        &libc_allocator,
        &libc_allocator,
        &ast,
        text,
        &libc_writer,
        &ldesc
    );

    mrv_gen_source(&libc_writer, &libc_writer, &scratch_allocator, &ldesc, text);

    mrv_ldesc_destroy(&ldesc);

out_destroy_ast:
    mrv_ast_destroy(&ast);
    mrv_tstream_destroy(&tstream, &libc_allocator);
    fclose(file);
out_free_all:
    lg_arena_free_all(&scratch_allocator);
    return ret_code;
}

#define LIBGRAD_IMPLEMENTATION
#include <libgrad/libgrad.h>
