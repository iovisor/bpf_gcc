/* Parser for C and Objective-C.
   Copyright (C) 1987-2015 Free Software Foundation, Inc.

   Parser actions based on the old Bison parser; structure somewhat
   influenced by and fragments based on the C++ parser.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* TODO:

   Make sure all relevant comments, and all relevant code from all
   actions, brought over from old parser.  Verify exact correspondence
   of syntax accepted.

   Add testcases covering every input symbol in every state in old and
   new parsers.

   Include full syntax for GNU C, including erroneous cases accepted
   with error messages, in syntax productions in comments.

   Make more diagnostics in the front end generally take an explicit
   location rather than implicitly using input_location.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"			/* For rtl.h: needs enum reg_class.  */
#include "tree.h"
#include "stringpool.h"
#include "attribs.h"
#include "stor-layout.h"
#include "varasm.h"
#include "trans-mem.h"
#include "langhooks.h"
#include "input.h"
#include "cpplib.h"
#include "timevar.h"
#include "c-family/c-pragma.h"
#include "c-tree.h"
#include "c-lang.h"
#include "flags.h"
#include "ggc.h"
#include "c-family/c-common.h"
#include "c-family/c-objc.h"
#include "vec.h"
#include "target.h"
#include "hash-map.h"
#include "is-a.h"
#include "plugin-api.h"
#include "hashtab.h"
#include "hash-set.h"
#include "machmode.h"
#include "hard-reg-set.h"
#include "function.h"
#include "ipa-ref.h"
#include "cgraph.h"
#include "plugin.h"
#include "omp-low.h"
#include "builtins.h"


/* Initialization routine for this file.  */

void
c_parse_init (void)
{
  /* The only initialization required is of the reserved word
     identifiers.  */
  unsigned int i;
  tree id;
  int mask = 0;

  /* Make sure RID_MAX hasn't grown past the 8 bits used to hold the keyword in
     the c_token structure.  */
  gcc_assert (RID_MAX <= 255);

  mask |= D_CXXONLY;
  if (!flag_isoc99)
    mask |= D_C99;
  if (flag_no_asm)
    {
      mask |= D_ASM | D_EXT;
      if (!flag_isoc99)
	mask |= D_EXT89;
    }
  if (!c_dialect_objc ())
    mask |= D_OBJC | D_CXX_OBJC;

  ridpointers = ggc_cleared_vec_alloc<tree> ((int) RID_MAX);
  for (i = 0; i < num_c_common_reswords; i++)
    {
      /* If a keyword is disabled, do not enter it into the table
	 and so create a canonical spelling that isn't a keyword.  */
      if (c_common_reswords[i].disable & mask)
	{
	  if (warn_cxx_compat
	      && (c_common_reswords[i].disable & D_CXXWARN))
	    {
	      id = get_identifier (c_common_reswords[i].word);
	      C_SET_RID_CODE (id, RID_CXX_COMPAT_WARN);
	      C_IS_RESERVED_WORD (id) = 1;
	    }
	  continue;
	}

      id = get_identifier (c_common_reswords[i].word);
      C_SET_RID_CODE (id, c_common_reswords[i].rid);
      C_IS_RESERVED_WORD (id) = 1;
      ridpointers [(int) c_common_reswords[i].rid] = id;
    }

  for (i = 0; i < NUM_INT_N_ENTS; i++)
    {
      /* We always create the symbols but they aren't always supported.  */
      char name[50];
      sprintf (name, "__int%d", int_n_data[i].bitsize);
      id = get_identifier (xstrdup (name));
      C_SET_RID_CODE (id, RID_FIRST_INT_N + i);
      C_IS_RESERVED_WORD (id) = 1;
    }
}

/* The C lexer intermediates between the lexer in cpplib and c-lex.c
   and the C parser.  Unlike the C++ lexer, the parser structure
   stores the lexer information instead of using a separate structure.
   Identifiers are separated into ordinary identifiers, type names,
   keywords and some other Objective-C types of identifiers, and some
   look-ahead is maintained.

   ??? It might be a good idea to lex the whole file up front (as for
   C++).  It would then be possible to share more of the C and C++
   lexer code, if desired.  */

/* More information about the type of a CPP_NAME token.  */
typedef enum c_id_kind {
  /* An ordinary identifier.  */
  C_ID_ID,
  /* An identifier declared as a typedef name.  */
  C_ID_TYPENAME,
  /* An identifier declared as an Objective-C class name.  */
  C_ID_CLASSNAME,
  /* An address space identifier.  */
  C_ID_ADDRSPACE,
  /* Not an identifier.  */
  C_ID_NONE
} c_id_kind;

/* A single C token after string literal concatenation and conversion
   of preprocessing tokens to tokens.  */
typedef struct GTY (()) c_token {
  /* The kind of token.  */
  ENUM_BITFIELD (cpp_ttype) type : 8;
  /* If this token is a CPP_NAME, this value indicates whether also
     declared as some kind of type.  Otherwise, it is C_ID_NONE.  */
  ENUM_BITFIELD (c_id_kind) id_kind : 8;
  /* If this token is a keyword, this value indicates which keyword.
     Otherwise, this value is RID_MAX.  */
  ENUM_BITFIELD (rid) keyword : 8;
  /* If this token is a CPP_PRAGMA, this indicates the pragma that
     was seen.  Otherwise it is PRAGMA_NONE.  */
  ENUM_BITFIELD (pragma_kind) pragma_kind : 8;
  /* The location at which this token was found.  */
  location_t location;
  /* The value associated with this token, if any.  */
  tree value;
} c_token;

/* A parser structure recording information about the state and
   context of parsing.  Includes lexer information with up to two
   tokens of look-ahead; more are not needed for C.  */
typedef struct GTY(()) c_parser {
  /* The look-ahead tokens.  */
  c_token * GTY((skip)) tokens;
  /* Buffer for look-ahead tokens.  */
  c_token tokens_buf[2];
  /* How many look-ahead tokens are available (0, 1 or 2, or
     more if parsing from pre-lexed tokens).  */
  unsigned int tokens_avail;
  /* True if a syntax error is being recovered from; false otherwise.
     c_parser_error sets this flag.  It should clear this flag when
     enough tokens have been consumed to recover from the error.  */
  BOOL_BITFIELD error : 1;
  /* True if we're processing a pragma, and shouldn't automatically
     consume CPP_PRAGMA_EOL.  */
  BOOL_BITFIELD in_pragma : 1;
  /* True if we're parsing the outermost block of an if statement.  */
  BOOL_BITFIELD in_if_block : 1;
  /* True if we want to lex an untranslated string.  */
  BOOL_BITFIELD lex_untranslated_string : 1;

  /* Objective-C specific parser/lexer information.  */

  /* True if we are in a context where the Objective-C "PQ" keywords
     are considered keywords.  */
  BOOL_BITFIELD objc_pq_context : 1;
  /* True if we are parsing a (potential) Objective-C foreach
     statement.  This is set to true after we parsed 'for (' and while
     we wait for 'in' or ';' to decide if it's a standard C for loop or an
     Objective-C foreach loop.  */
  BOOL_BITFIELD objc_could_be_foreach_context : 1;
  /* The following flag is needed to contextualize Objective-C lexical
     analysis.  In some cases (e.g., 'int NSObject;'), it is
     undesirable to bind an identifier to an Objective-C class, even
     if a class with that name exists.  */
  BOOL_BITFIELD objc_need_raw_identifier : 1;
  /* Nonzero if we're processing a __transaction statement.  The value
     is 1 | TM_STMT_ATTR_*.  */
  unsigned int in_transaction : 4;
  /* True if we are in a context where the Objective-C "Property attribute"
     keywords are valid.  */
  BOOL_BITFIELD objc_property_attr_context : 1;

  /* Cilk Plus specific parser/lexer information.  */

  /* Buffer to hold all the tokens from parsing the vector attribute for the
     SIMD-enabled functions (formerly known as elemental functions).  */
  vec <c_token, va_gc> *cilk_simd_fn_tokens;
} c_parser;


/* The actual parser and external interface.  ??? Does this need to be
   garbage-collected?  */

static GTY (()) c_parser *the_parser;

/* Read in and lex a single token, storing it in *TOKEN.  */

static void
c_lex_one_token (c_parser *parser, c_token *token)
{
  timevar_push (TV_LEX);

  token->type = c_lex_with_flags (&token->value, &token->location, NULL,
				  (parser->lex_untranslated_string
				   ? C_LEX_STRING_NO_TRANSLATE : 0));
  token->id_kind = C_ID_NONE;
  token->keyword = RID_MAX;
  token->pragma_kind = PRAGMA_NONE;

  switch (token->type)
    {
    case CPP_NAME:
      {
	tree decl;

	bool objc_force_identifier = parser->objc_need_raw_identifier;
	if (c_dialect_objc ())
	  parser->objc_need_raw_identifier = false;

	if (C_IS_RESERVED_WORD (token->value))
	  {
	    enum rid rid_code = C_RID_CODE (token->value);

	    if (rid_code == RID_CXX_COMPAT_WARN)
	      {
		warning_at (token->location,
			    OPT_Wc___compat,
			    "identifier %qE conflicts with C++ keyword",
			    token->value);
	      }
	    else if (rid_code >= RID_FIRST_ADDR_SPACE
		     && rid_code <= RID_LAST_ADDR_SPACE)
	      {
		token->id_kind = C_ID_ADDRSPACE;
		token->keyword = rid_code;
		break;
	      }
	    else if (c_dialect_objc () && OBJC_IS_PQ_KEYWORD (rid_code))
	      {
		/* We found an Objective-C "pq" keyword (in, out,
		   inout, bycopy, byref, oneway).  They need special
		   care because the interpretation depends on the
		   context.  */
		if (parser->objc_pq_context)
		  {
		    token->type = CPP_KEYWORD;
		    token->keyword = rid_code;
		    break;
		  }
		else if (parser->objc_could_be_foreach_context
			 && rid_code == RID_IN)
		  {
		    /* We are in Objective-C, inside a (potential)
		       foreach context (which means after having
		       parsed 'for (', but before having parsed ';'),
		       and we found 'in'.  We consider it the keyword
		       which terminates the declaration at the
		       beginning of a foreach-statement.  Note that
		       this means you can't use 'in' for anything else
		       in that context; in particular, in Objective-C
		       you can't use 'in' as the name of the running
		       variable in a C for loop.  We could potentially
		       try to add code here to disambiguate, but it
		       seems a reasonable limitation.  */
		    token->type = CPP_KEYWORD;
		    token->keyword = rid_code;
		    break;
		  }
		/* Else, "pq" keywords outside of the "pq" context are
		   not keywords, and we fall through to the code for
		   normal tokens.  */
	      }
	    else if (c_dialect_objc () && OBJC_IS_PATTR_KEYWORD (rid_code))
	      {
		/* We found an Objective-C "property attribute"
		   keyword (getter, setter, readonly, etc). These are
		   only valid in the property context.  */
		if (parser->objc_property_attr_context)
		  {
		    token->type = CPP_KEYWORD;
		    token->keyword = rid_code;
		    break;
		  }
		/* Else they are not special keywords.
		*/
	      }
	    else if (c_dialect_objc () 
		     && (OBJC_IS_AT_KEYWORD (rid_code)
			 || OBJC_IS_CXX_KEYWORD (rid_code)))
	      {
		/* We found one of the Objective-C "@" keywords (defs,
		   selector, synchronized, etc) or one of the
		   Objective-C "cxx" keywords (class, private,
		   protected, public, try, catch, throw) without a
		   preceding '@' sign.  Do nothing and fall through to
		   the code for normal tokens (in C++ we would still
		   consider the CXX ones keywords, but not in C).  */
		;
	      }
	    else
	      {
		token->type = CPP_KEYWORD;
		token->keyword = rid_code;
		break;
	      }
	  }

	decl = lookup_name (token->value);
	if (decl)
	  {
	    if (TREE_CODE (decl) == TYPE_DECL)
	      {
		token->id_kind = C_ID_TYPENAME;
		break;
	      }
	  }
	else if (c_dialect_objc ())
	  {
	    tree objc_interface_decl = objc_is_class_name (token->value);
	    /* Objective-C class names are in the same namespace as
	       variables and typedefs, and hence are shadowed by local
	       declarations.  */
	    if (objc_interface_decl
                && (!objc_force_identifier || global_bindings_p ()))
	      {
		token->value = objc_interface_decl;
		token->id_kind = C_ID_CLASSNAME;
		break;
	      }
	  }
        token->id_kind = C_ID_ID;
      }
      break;
    case CPP_AT_NAME:
      /* This only happens in Objective-C; it must be a keyword.  */
      token->type = CPP_KEYWORD;
      switch (C_RID_CODE (token->value))
	{
	  /* Replace 'class' with '@class', 'private' with '@private',
	     etc.  This prevents confusion with the C++ keyword
	     'class', and makes the tokens consistent with other
	     Objective-C 'AT' keywords.  For example '@class' is
	     reported as RID_AT_CLASS which is consistent with
	     '@synchronized', which is reported as
	     RID_AT_SYNCHRONIZED.
	  */
	case RID_CLASS:     token->keyword = RID_AT_CLASS; break;
	case RID_PRIVATE:   token->keyword = RID_AT_PRIVATE; break;
	case RID_PROTECTED: token->keyword = RID_AT_PROTECTED; break;
	case RID_PUBLIC:    token->keyword = RID_AT_PUBLIC; break;
	case RID_THROW:     token->keyword = RID_AT_THROW; break;
	case RID_TRY:       token->keyword = RID_AT_TRY; break;
	case RID_CATCH:     token->keyword = RID_AT_CATCH; break;
	default:            token->keyword = C_RID_CODE (token->value);
	}
      break;
    case CPP_COLON:
    case CPP_COMMA:
    case CPP_CLOSE_PAREN:
    case CPP_SEMICOLON:
      /* These tokens may affect the interpretation of any identifiers
	 following, if doing Objective-C.  */
      if (c_dialect_objc ())
	parser->objc_need_raw_identifier = false;
      break;
    case CPP_PRAGMA:
      /* We smuggled the cpp_token->u.pragma value in an INTEGER_CST.  */
      token->pragma_kind = (enum pragma_kind) TREE_INT_CST_LOW (token->value);
      token->value = NULL;
      break;
    default:
      break;
    }
  timevar_pop (TV_LEX);
}

/* Return a pointer to the next token from PARSER, reading it in if
   necessary.  */

static inline c_token *
c_parser_peek_token (c_parser *parser)
{
  if (parser->tokens_avail == 0)
    {
      c_lex_one_token (parser, &parser->tokens[0]);
      parser->tokens_avail = 1;
    }
  return &parser->tokens[0];
}

/* Return true if the next token from PARSER has the indicated
   TYPE.  */

static inline bool
c_parser_next_token_is (c_parser *parser, enum cpp_ttype type)
{
  return c_parser_peek_token (parser)->type == type;
}

/* Return true if the next token from PARSER does not have the
   indicated TYPE.  */

static inline bool
c_parser_next_token_is_not (c_parser *parser, enum cpp_ttype type)
{
  return !c_parser_next_token_is (parser, type);
}

/* Return true if the next token from PARSER is the indicated
   KEYWORD.  */

static inline bool
c_parser_next_token_is_keyword (c_parser *parser, enum rid keyword)
{
  return c_parser_peek_token (parser)->keyword == keyword;
}

/* Return a pointer to the next-but-one token from PARSER, reading it
   in if necessary.  The next token is already read in.  */

static c_token *
c_parser_peek_2nd_token (c_parser *parser)
{
  if (parser->tokens_avail >= 2)
    return &parser->tokens[1];
  gcc_assert (parser->tokens_avail == 1);
  gcc_assert (parser->tokens[0].type != CPP_EOF);
  gcc_assert (parser->tokens[0].type != CPP_PRAGMA_EOL);
  c_lex_one_token (parser, &parser->tokens[1]);
  parser->tokens_avail = 2;
  return &parser->tokens[1];
}

/* Return true if TOKEN can start a type name,
   false otherwise.  */
static bool
c_token_starts_typename (c_token *token)
{
  switch (token->type)
    {
    case CPP_NAME:
      switch (token->id_kind)
	{
	case C_ID_ID:
	  return false;
	case C_ID_ADDRSPACE:
	  return true;
	case C_ID_TYPENAME:
	  return true;
	case C_ID_CLASSNAME:
	  gcc_assert (c_dialect_objc ());
	  return true;
	default:
	  gcc_unreachable ();
	}
    case CPP_KEYWORD:
      switch (token->keyword)
	{
	case RID_UNSIGNED:
	case RID_LONG:
	case RID_SHORT:
	case RID_SIGNED:
	case RID_COMPLEX:
	case RID_INT:
	case RID_CHAR:
	case RID_FLOAT:
	case RID_DOUBLE:
	case RID_VOID:
	case RID_DFLOAT32:
	case RID_DFLOAT64:
	case RID_DFLOAT128:
	case RID_BOOL:
	case RID_ENUM:
	case RID_STRUCT:
	case RID_UNION:
	case RID_TYPEOF:
	case RID_CONST:
	case RID_ATOMIC:
	case RID_VOLATILE:
	case RID_RESTRICT:
	case RID_ATTRIBUTE:
	case RID_FRACT:
	case RID_ACCUM:
	case RID_SAT:
	case RID_AUTO_TYPE:
	  return true;
	default:
	  if (token->keyword >= RID_FIRST_INT_N
	      && token->keyword < RID_FIRST_INT_N + NUM_INT_N_ENTS
	      && int_n_enabled_p[token->keyword - RID_FIRST_INT_N])
	    return true;
	  return false;
	}
    case CPP_LESS:
      if (c_dialect_objc ())
	return true;
      return false;
    default:
      return false;
    }
}

enum c_lookahead_kind {
  /* Always treat unknown identifiers as typenames.  */
  cla_prefer_type,

  /* Could be parsing a nonabstract declarator.  Only treat an identifier
     as a typename if followed by another identifier or a star.  */
  cla_nonabstract_decl,

  /* Never treat identifiers as typenames.  */
  cla_prefer_id
};

/* Return true if the next token from PARSER can start a type name,
   false otherwise.  LA specifies how to do lookahead in order to
   detect unknown type names.  If unsure, pick CLA_PREFER_ID.  */

static inline bool
c_parser_next_tokens_start_typename (c_parser *parser, enum c_lookahead_kind la)
{
  c_token *token = c_parser_peek_token (parser);
  if (c_token_starts_typename (token))
    return true;

  /* Try a bit harder to detect an unknown typename.  */
  if (la != cla_prefer_id
      && token->type == CPP_NAME
      && token->id_kind == C_ID_ID

      /* Do not try too hard when we could have "object in array".  */
      && !parser->objc_could_be_foreach_context

      && (la == cla_prefer_type
	  || c_parser_peek_2nd_token (parser)->type == CPP_NAME
	  || c_parser_peek_2nd_token (parser)->type == CPP_MULT)

      /* Only unknown identifiers.  */
      && !lookup_name (token->value))
    return true;

  return false;
}

/* Return true if TOKEN is a type qualifier, false otherwise.  */
static bool
c_token_is_qualifier (c_token *token)
{
  switch (token->type)
    {
    case CPP_NAME:
      switch (token->id_kind)
	{
	case C_ID_ADDRSPACE:
	  return true;
	default:
	  return false;
	}
    case CPP_KEYWORD:
      switch (token->keyword)
	{
	case RID_CONST:
	case RID_VOLATILE:
	case RID_RESTRICT:
	case RID_ATTRIBUTE:
	case RID_ATOMIC:
	  return true;
	default:
	  return false;
	}
    case CPP_LESS:
      return false;
    default:
      gcc_unreachable ();
    }
}

/* Return true if the next token from PARSER is a type qualifier,
   false otherwise.  */
static inline bool
c_parser_next_token_is_qualifier (c_parser *parser)
{
  c_token *token = c_parser_peek_token (parser);
  return c_token_is_qualifier (token);
}

/* Return true if TOKEN can start declaration specifiers, false
   otherwise.  */
static bool
c_token_starts_declspecs (c_token *token)
{
  switch (token->type)
    {
    case CPP_NAME:
      switch (token->id_kind)
	{
	case C_ID_ID:
	  return false;
	case C_ID_ADDRSPACE:
	  return true;
	case C_ID_TYPENAME:
	  return true;
	case C_ID_CLASSNAME:
	  gcc_assert (c_dialect_objc ());
	  return true;
	default:
	  gcc_unreachable ();
	}
    case CPP_KEYWORD:
      switch (token->keyword)
	{
	case RID_STATIC:
	case RID_EXTERN:
	case RID_REGISTER:
	case RID_TYPEDEF:
	case RID_INLINE:
	case RID_NORETURN:
	case RID_AUTO:
	case RID_THREAD:
	case RID_UNSIGNED:
	case RID_LONG:
	case RID_SHORT:
	case RID_SIGNED:
	case RID_COMPLEX:
	case RID_INT:
	case RID_CHAR:
	case RID_FLOAT:
	case RID_DOUBLE:
	case RID_VOID:
	case RID_DFLOAT32:
	case RID_DFLOAT64:
	case RID_DFLOAT128:
	case RID_BOOL:
	case RID_ENUM:
	case RID_STRUCT:
	case RID_UNION:
	case RID_TYPEOF:
	case RID_CONST:
	case RID_VOLATILE:
	case RID_RESTRICT:
	case RID_ATTRIBUTE:
	case RID_FRACT:
	case RID_ACCUM:
	case RID_SAT:
	case RID_ALIGNAS:
	case RID_ATOMIC:
	case RID_AUTO_TYPE:
	  return true;
	default:
	  if (token->keyword >= RID_FIRST_INT_N
	      && token->keyword < RID_FIRST_INT_N + NUM_INT_N_ENTS
	      && int_n_enabled_p[token->keyword - RID_FIRST_INT_N])
	    return true;
	  return false;
	}
    case CPP_LESS:
      if (c_dialect_objc ())
	return true;
      return false;
    default:
      return false;
    }
}


/* Return true if TOKEN can start declaration specifiers or a static
   assertion, false otherwise.  */
static bool
c_token_starts_declaration (c_token *token)
{
  if (c_token_starts_declspecs (token)
      || token->keyword == RID_STATIC_ASSERT)
    return true;
  else
    return false;
}

/* Return true if the next token from PARSER can start declaration
   specifiers, false otherwise.  */
static inline bool
c_parser_next_token_starts_declspecs (c_parser *parser)
{
  c_token *token = c_parser_peek_token (parser);

  /* In Objective-C, a classname normally starts a declspecs unless it
     is immediately followed by a dot.  In that case, it is the
     Objective-C 2.0 "dot-syntax" for class objects, ie, calls the
     setter/getter on the class.  c_token_starts_declspecs() can't
     differentiate between the two cases because it only checks the
     current token, so we have a special check here.  */
  if (c_dialect_objc () 
      && token->type == CPP_NAME
      && token->id_kind == C_ID_CLASSNAME 
      && c_parser_peek_2nd_token (parser)->type == CPP_DOT)
    return false;

  return c_token_starts_declspecs (token);
}

/* Return true if the next tokens from PARSER can start declaration
   specifiers or a static assertion, false otherwise.  */
static inline bool
c_parser_next_tokens_start_declaration (c_parser *parser)
{
  c_token *token = c_parser_peek_token (parser);

  /* Same as above.  */
  if (c_dialect_objc () 
      && token->type == CPP_NAME
      && token->id_kind == C_ID_CLASSNAME 
      && c_parser_peek_2nd_token (parser)->type == CPP_DOT)
    return false;

  /* Labels do not start declarations.  */
  if (token->type == CPP_NAME
      && c_parser_peek_2nd_token (parser)->type == CPP_COLON)
    return false;

  if (c_token_starts_declaration (token))
    return true;

  if (c_parser_next_tokens_start_typename (parser, cla_nonabstract_decl))
    return true;

  return false;
}

/* Consume the next token from PARSER.  */

static void
c_parser_consume_token (c_parser *parser)
{
  gcc_assert (parser->tokens_avail >= 1);
  gcc_assert (parser->tokens[0].type != CPP_EOF);
  gcc_assert (!parser->in_pragma || parser->tokens[0].type != CPP_PRAGMA_EOL);
  gcc_assert (parser->error || parser->tokens[0].type != CPP_PRAGMA);
  if (parser->tokens != &parser->tokens_buf[0])
    parser->tokens++;
  else if (parser->tokens_avail == 2)
    parser->tokens[0] = parser->tokens[1];
  parser->tokens_avail--;
}

/* Expect the current token to be a #pragma.  Consume it and remember
   that we've begun parsing a pragma.  */

static void
c_parser_consume_pragma (c_parser *parser)
{
  gcc_assert (!parser->in_pragma);
  gcc_assert (parser->tokens_avail >= 1);
  gcc_assert (parser->tokens[0].type == CPP_PRAGMA);
  if (parser->tokens != &parser->tokens_buf[0])
    parser->tokens++;
  else if (parser->tokens_avail == 2)
    parser->tokens[0] = parser->tokens[1];
  parser->tokens_avail--;
  parser->in_pragma = true;
}

/* Update the global input_location from TOKEN.  */
static inline void
c_parser_set_source_position_from_token (c_token *token)
{
  if (token->type != CPP_EOF)
    {
      input_location = token->location;
    }
}

/* Issue a diagnostic of the form
      FILE:LINE: MESSAGE before TOKEN
   where TOKEN is the next token in the input stream of PARSER.
   MESSAGE (specified by the caller) is usually of the form "expected
   OTHER-TOKEN".

   Do not issue a diagnostic if still recovering from an error.

   ??? This is taken from the C++ parser, but building up messages in
   this way is not i18n-friendly and some other approach should be
   used.  */

static void
c_parser_error (c_parser *parser, const char *gmsgid)
{
  c_token *token = c_parser_peek_token (parser);
  if (parser->error)
    return;
  parser->error = true;
  if (!gmsgid)
    return;
  /* This diagnostic makes more sense if it is tagged to the line of
     the token we just peeked at.  */
  c_parser_set_source_position_from_token (token);
  c_parse_error (gmsgid,
		 /* Because c_parse_error does not understand
		    CPP_KEYWORD, keywords are treated like
		    identifiers.  */
		 (token->type == CPP_KEYWORD ? CPP_NAME : token->type),
		 /* ??? The C parser does not save the cpp flags of a
		    token, we need to pass 0 here and we will not get
		    the source spelling of some tokens but rather the
		    canonical spelling.  */
		 token->value, /*flags=*/0);
}

/* If the next token is of the indicated TYPE, consume it.  Otherwise,
   issue the error MSGID.  If MSGID is NULL then a message has already
   been produced and no message will be produced this time.  Returns
   true if found, false otherwise.  */

static bool
c_parser_require (c_parser *parser,
		  enum cpp_ttype type,
		  const char *msgid)
{
  if (c_parser_next_token_is (parser, type))
    {
      c_parser_consume_token (parser);
      return true;
    }
  else
    {
      c_parser_error (parser, msgid);
      return false;
    }
}

/* If the next token is the indicated keyword, consume it.  Otherwise,
   issue the error MSGID.  Returns true if found, false otherwise.  */

static bool
c_parser_require_keyword (c_parser *parser,
			  enum rid keyword,
			  const char *msgid)
{
  if (c_parser_next_token_is_keyword (parser, keyword))
    {
      c_parser_consume_token (parser);
      return true;
    }
  else
    {
      c_parser_error (parser, msgid);
      return false;
    }
}

/* Like c_parser_require, except that tokens will be skipped until the
   desired token is found.  An error message is still produced if the
   next token is not as expected.  If MSGID is NULL then a message has
   already been produced and no message will be produced this
   time.  */

static void
c_parser_skip_until_found (c_parser *parser,
			   enum cpp_ttype type,
			   const char *msgid)
{
  unsigned nesting_depth = 0;

  if (c_parser_require (parser, type, msgid))
    return;

  /* Skip tokens until the desired token is found.  */
  while (true)
    {
      /* Peek at the next token.  */
      c_token *token = c_parser_peek_token (parser);
      /* If we've reached the token we want, consume it and stop.  */
      if (token->type == type && !nesting_depth)
	{
	  c_parser_consume_token (parser);
	  break;
	}

      /* If we've run out of tokens, stop.  */
      if (token->type == CPP_EOF)
	return;
      if (token->type == CPP_PRAGMA_EOL && parser->in_pragma)
	return;
      if (token->type == CPP_OPEN_BRACE
	  || token->type == CPP_OPEN_PAREN
	  || token->type == CPP_OPEN_SQUARE)
	++nesting_depth;
      else if (token->type == CPP_CLOSE_BRACE
	       || token->type == CPP_CLOSE_PAREN
	       || token->type == CPP_CLOSE_SQUARE)
	{
	  if (nesting_depth-- == 0)
	    break;
	}
      /* Consume this token.  */
      c_parser_consume_token (parser);
    }
  parser->error = false;
}

/* Skip tokens until the end of a parameter is found, but do not
   consume the comma, semicolon or closing delimiter.  */

static void
c_parser_skip_to_end_of_parameter (c_parser *parser)
{
  unsigned nesting_depth = 0;

  while (true)
    {
      c_token *token = c_parser_peek_token (parser);
      if ((token->type == CPP_COMMA || token->type == CPP_SEMICOLON)
	  && !nesting_depth)
	break;
      /* If we've run out of tokens, stop.  */
      if (token->type == CPP_EOF)
	return;
      if (token->type == CPP_PRAGMA_EOL && parser->in_pragma)
	return;
      if (token->type == CPP_OPEN_BRACE
	  || token->type == CPP_OPEN_PAREN
	  || token->type == CPP_OPEN_SQUARE)
	++nesting_depth;
      else if (token->type == CPP_CLOSE_BRACE
	       || token->type == CPP_CLOSE_PAREN
	       || token->type == CPP_CLOSE_SQUARE)
	{
	  if (nesting_depth-- == 0)
	    break;
	}
      /* Consume this token.  */
      c_parser_consume_token (parser);
    }
  parser->error = false;
}

/* Expect to be at the end of the pragma directive and consume an
   end of line marker.  */

static void
c_parser_skip_to_pragma_eol (c_parser *parser)
{
  gcc_assert (parser->in_pragma);
  parser->in_pragma = false;

  if (!c_parser_require (parser, CPP_PRAGMA_EOL, "expected end of line"))
    while (true)
      {
	c_token *token = c_parser_peek_token (parser);
	if (token->type == CPP_EOF)
	  break;
	if (token->type == CPP_PRAGMA_EOL)
	  {
	    c_parser_consume_token (parser);
	    break;
	  }
	c_parser_consume_token (parser);
      }

  parser->error = false;
}

/* Skip tokens until we have consumed an entire block, or until we
   have consumed a non-nested ';'.  */

static void
c_parser_skip_to_end_of_block_or_statement (c_parser *parser)
{
  unsigned nesting_depth = 0;
  bool save_error = parser->error;

  while (true)
    {
      c_token *token;

      /* Peek at the next token.  */
      token = c_parser_peek_token (parser);

      switch (token->type)
	{
	case CPP_EOF:
	  return;

	case CPP_PRAGMA_EOL:
	  if (parser->in_pragma)
	    return;
	  break;

	case CPP_SEMICOLON:
	  /* If the next token is a ';', we have reached the
	     end of the statement.  */
	  if (!nesting_depth)
	    {
	      /* Consume the ';'.  */
	      c_parser_consume_token (parser);
	      goto finished;
	    }
	  break;

	case CPP_CLOSE_BRACE:
	  /* If the next token is a non-nested '}', then we have
	     reached the end of the current block.  */
	  if (nesting_depth == 0 || --nesting_depth == 0)
	    {
	      c_parser_consume_token (parser);
	      goto finished;
	    }
	  break;

	case CPP_OPEN_BRACE:
	  /* If it the next token is a '{', then we are entering a new
	     block.  Consume the entire block.  */
	  ++nesting_depth;
	  break;

	case CPP_PRAGMA:
	  /* If we see a pragma, consume the whole thing at once.  We
	     have some safeguards against consuming pragmas willy-nilly.
	     Normally, we'd expect to be here with parser->error set,
	     which disables these safeguards.  But it's possible to get
	     here for secondary error recovery, after parser->error has
	     been cleared.  */
	  c_parser_consume_pragma (parser);
	  c_parser_skip_to_pragma_eol (parser);
	  parser->error = save_error;
	  continue;

	default:
	  break;
	}

      c_parser_consume_token (parser);
    }

 finished:
  parser->error = false;
}

/* CPP's options (initialized by c-opts.c).  */
extern cpp_options *cpp_opts;

/* Save the warning flags which are controlled by __extension__.  */

static inline int
disable_extension_diagnostics (void)
{
  int ret = (pedantic
	     | (warn_pointer_arith << 1)
	     | (warn_traditional << 2)
	     | (flag_iso << 3)
	     | (warn_long_long << 4)
	     | (warn_cxx_compat << 5)
	     | (warn_overlength_strings << 6)
	     /* warn_c90_c99_compat has three states: -1/0/1, so we must
		play tricks to properly restore it.  */
	     | ((warn_c90_c99_compat == 1) << 7)
	     | ((warn_c90_c99_compat == -1) << 8)
	     /* Similarly for warn_c99_c11_compat.  */
	     | ((warn_c99_c11_compat == 1) << 9)
	     | ((warn_c99_c11_compat == -1) << 10)
	     );
  cpp_opts->cpp_pedantic = pedantic = 0;
  warn_pointer_arith = 0;
  cpp_opts->cpp_warn_traditional = warn_traditional = 0;
  flag_iso = 0;
  cpp_opts->cpp_warn_long_long = warn_long_long = 0;
  warn_cxx_compat = 0;
  warn_overlength_strings = 0;
  warn_c90_c99_compat = 0;
  warn_c99_c11_compat = 0;
  return ret;
}

/* Restore the warning flags which are controlled by __extension__.
   FLAGS is the return value from disable_extension_diagnostics.  */

static inline void
restore_extension_diagnostics (int flags)
{
  cpp_opts->cpp_pedantic = pedantic = flags & 1;
  warn_pointer_arith = (flags >> 1) & 1;
  cpp_opts->cpp_warn_traditional = warn_traditional = (flags >> 2) & 1;
  flag_iso = (flags >> 3) & 1;
  cpp_opts->cpp_warn_long_long = warn_long_long = (flags >> 4) & 1;
  warn_cxx_compat = (flags >> 5) & 1;
  warn_overlength_strings = (flags >> 6) & 1;
  /* See above for why is this needed.  */
  warn_c90_c99_compat = (flags >> 7) & 1 ? 1 : ((flags >> 8) & 1 ? -1 : 0);
  warn_c99_c11_compat = (flags >> 9) & 1 ? 1 : ((flags >> 10) & 1 ? -1 : 0);
}

/* Possibly kinds of declarator to parse.  */
typedef enum c_dtr_syn {
  /* A normal declarator with an identifier.  */
  C_DTR_NORMAL,
  /* An abstract declarator (maybe empty).  */
  C_DTR_ABSTRACT,
  /* A parameter declarator: may be either, but after a type name does
     not redeclare a typedef name as an identifier if it can
     alternatively be interpreted as a typedef name; see DR#009,
     applied in C90 TC1, omitted from C99 and reapplied in C99 TC2
     following DR#249.  For example, given a typedef T, "int T" and
     "int *T" are valid parameter declarations redeclaring T, while
     "int (T)" and "int * (T)" and "int (T[])" and "int (T (int))" are
     abstract declarators rather than involving redundant parentheses;
     the same applies with attributes inside the parentheses before
     "T".  */
  C_DTR_PARM
} c_dtr_syn;

/* The binary operation precedence levels, where 0 is a dummy lowest level
   used for the bottom of the stack.  */
enum c_parser_prec {
  PREC_NONE,
  PREC_LOGOR,
  PREC_LOGAND,
  PREC_BITOR,
  PREC_BITXOR,
  PREC_BITAND,
  PREC_EQ,
  PREC_REL,
  PREC_SHIFT,
  PREC_ADD,
  PREC_MULT,
  NUM_PRECS
};

static void c_parser_external_declaration (c_parser *);
static void c_parser_asm_definition (c_parser *);
static void c_parser_declaration_or_fndef (c_parser *, bool, bool, bool,
					   bool, bool, tree *, vec<c_token>);
static void c_parser_static_assert_declaration_no_semi (c_parser *);
static void c_parser_static_assert_declaration (c_parser *);
static void c_parser_declspecs (c_parser *, struct c_declspecs *, bool, bool,
				bool, bool, bool, enum c_lookahead_kind);
static struct c_typespec c_parser_enum_specifier (c_parser *);
static struct c_typespec c_parser_struct_or_union_specifier (c_parser *);
static tree c_parser_struct_declaration (c_parser *);
static struct c_typespec c_parser_typeof_specifier (c_parser *);
static tree c_parser_alignas_specifier (c_parser *);
static struct c_declarator *c_parser_declarator (c_parser *, bool, c_dtr_syn,
						 bool *);
static struct c_declarator *c_parser_direct_declarator (c_parser *, bool,
							c_dtr_syn, bool *);
static struct c_declarator *c_parser_direct_declarator_inner (c_parser *,
							      bool,
							      struct c_declarator *);
static struct c_arg_info *c_parser_parms_declarator (c_parser *, bool, tree);
static struct c_arg_info *c_parser_parms_list_declarator (c_parser *, tree,
							  tree);
static struct c_parm *c_parser_parameter_declaration (c_parser *, tree);
static tree c_parser_simple_asm_expr (c_parser *);
static tree c_parser_attributes (c_parser *);
static struct c_type_name *c_parser_type_name (c_parser *);
static struct c_expr c_parser_initializer (c_parser *);
static struct c_expr c_parser_braced_init (c_parser *, tree, bool);
static void c_parser_initelt (c_parser *, struct obstack *);
static void c_parser_initval (c_parser *, struct c_expr *,
			      struct obstack *);
static tree c_parser_compound_statement (c_parser *);
static void c_parser_compound_statement_nostart (c_parser *);
static void c_parser_label (c_parser *);
static void c_parser_statement (c_parser *);
static void c_parser_statement_after_labels (c_parser *);
static void c_parser_if_statement (c_parser *);
static void c_parser_switch_statement (c_parser *);
static void c_parser_while_statement (c_parser *, bool);
static void c_parser_do_statement (c_parser *, bool);
static void c_parser_for_statement (c_parser *, bool);
static tree c_parser_asm_statement (c_parser *);
static tree c_parser_asm_operands (c_parser *);
static tree c_parser_asm_goto_operands (c_parser *);
static tree c_parser_asm_clobbers (c_parser *);
static struct c_expr c_parser_expr_no_commas (c_parser *, struct c_expr *,
					      tree = NULL_TREE);
static struct c_expr c_parser_conditional_expression (c_parser *,
						      struct c_expr *, tree);
static struct c_expr c_parser_binary_expression (c_parser *, struct c_expr *,
						 tree);
static struct c_expr c_parser_cast_expression (c_parser *, struct c_expr *);
static struct c_expr c_parser_unary_expression (c_parser *);
static struct c_expr c_parser_sizeof_expression (c_parser *);
static struct c_expr c_parser_alignof_expression (c_parser *);
static struct c_expr c_parser_postfix_expression (c_parser *);
static struct c_expr c_parser_postfix_expression_after_paren_type (c_parser *,
								   struct c_type_name *,
								   location_t);
static struct c_expr c_parser_postfix_expression_after_primary (c_parser *,
								location_t loc,
								struct c_expr);
static tree c_parser_transaction (c_parser *, enum rid);
static struct c_expr c_parser_transaction_expression (c_parser *, enum rid);
static tree c_parser_transaction_cancel (c_parser *);
static struct c_expr c_parser_expression (c_parser *);
static struct c_expr c_parser_expression_conv (c_parser *);
static vec<tree, va_gc> *c_parser_expr_list (c_parser *, bool, bool,
					     vec<tree, va_gc> **, location_t *,
					     tree *, vec<location_t> *,
					     unsigned int * = NULL);
static void c_parser_omp_construct (c_parser *);
static void c_parser_omp_threadprivate (c_parser *);
static void c_parser_omp_barrier (c_parser *);
static void c_parser_omp_flush (c_parser *);
static void c_parser_omp_taskwait (c_parser *);
static void c_parser_omp_taskyield (c_parser *);
static void c_parser_omp_cancel (c_parser *);
static void c_parser_omp_cancellation_point (c_parser *);

enum pragma_context { pragma_external, pragma_struct, pragma_param,
		      pragma_stmt, pragma_compound };
static bool c_parser_pragma (c_parser *, enum pragma_context);
static bool c_parser_omp_target (c_parser *, enum pragma_context);
static void c_parser_omp_end_declare_target (c_parser *);
static void c_parser_omp_declare (c_parser *, enum pragma_context);

/* These Objective-C parser functions are only ever called when
   compiling Objective-C.  */
static void c_parser_objc_class_definition (c_parser *, tree);
static void c_parser_objc_class_instance_variables (c_parser *);
static void c_parser_objc_class_declaration (c_parser *);
static void c_parser_objc_alias_declaration (c_parser *);
static void c_parser_objc_protocol_definition (c_parser *, tree);
static bool c_parser_objc_method_type (c_parser *);
static void c_parser_objc_method_definition (c_parser *);
static void c_parser_objc_methodprotolist (c_parser *);
static void c_parser_objc_methodproto (c_parser *);
static tree c_parser_objc_method_decl (c_parser *, bool, tree *, tree *);
static tree c_parser_objc_type_name (c_parser *);
static tree c_parser_objc_protocol_refs (c_parser *);
static void c_parser_objc_try_catch_finally_statement (c_parser *);
static void c_parser_objc_synchronized_statement (c_parser *);
static tree c_parser_objc_selector (c_parser *);
static tree c_parser_objc_selector_arg (c_parser *);
static tree c_parser_objc_receiver (c_parser *);
static tree c_parser_objc_message_args (c_parser *);
static tree c_parser_objc_keywordexpr (c_parser *);
static void c_parser_objc_at_property_declaration (c_parser *);
static void c_parser_objc_at_synthesize_declaration (c_parser *);
static void c_parser_objc_at_dynamic_declaration (c_parser *);
static bool c_parser_objc_diagnose_bad_element_prefix
  (c_parser *, struct c_declspecs *);

/* Cilk Plus supporting routines.  */
static void c_parser_cilk_simd (c_parser *);
static void c_parser_cilk_for (c_parser *, tree);
static bool c_parser_cilk_verify_simd (c_parser *, enum pragma_context);
static tree c_parser_array_notation (location_t, c_parser *, tree, tree);
static tree c_parser_cilk_clause_vectorlength (c_parser *, tree, bool);
static void c_parser_cilk_grainsize (c_parser *);

/* Parse a translation unit (C90 6.7, C99 6.9).

   translation-unit:
     external-declarations

   external-declarations:
     external-declaration
     external-declarations external-declaration

   GNU extensions:

   translation-unit:
     empty
*/

static void
c_parser_translation_unit (c_parser *parser)
{
  if (c_parser_next_token_is (parser, CPP_EOF))
    {
      pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
	       "ISO C forbids an empty translation unit");
    }
  else
    {
      void *obstack_position = obstack_alloc (&parser_obstack, 0);
      mark_valid_location_for_stdc_pragma (false);
      do
	{
	  ggc_collect ();
	  c_parser_external_declaration (parser);
	  obstack_free (&parser_obstack, obstack_position);
	}
      while (c_parser_next_token_is_not (parser, CPP_EOF));
    }
}

/* Parse an external declaration (C90 6.7, C99 6.9).

   external-declaration:
     function-definition
     declaration

   GNU extensions:

   external-declaration:
     asm-definition
     ;
     __extension__ external-declaration

   Objective-C:

   external-declaration:
     objc-class-definition
     objc-class-declaration
     objc-alias-declaration
     objc-protocol-definition
     objc-method-definition
     @end
*/

static void
c_parser_external_declaration (c_parser *parser)
{
  int ext;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_EXTENSION:
	  ext = disable_extension_diagnostics ();
	  c_parser_consume_token (parser);
	  c_parser_external_declaration (parser);
	  restore_extension_diagnostics (ext);
	  break;
	case RID_ASM:
	  c_parser_asm_definition (parser);
	  break;
	case RID_AT_INTERFACE:
	case RID_AT_IMPLEMENTATION:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_class_definition (parser, NULL_TREE);
	  break;
	case RID_AT_CLASS:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_class_declaration (parser);
	  break;
	case RID_AT_ALIAS:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_alias_declaration (parser);
	  break;
	case RID_AT_PROTOCOL:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_protocol_definition (parser, NULL_TREE);
	  break;
	case RID_AT_PROPERTY:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_at_property_declaration (parser);
	  break;
	case RID_AT_SYNTHESIZE:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_at_synthesize_declaration (parser);
	  break;
	case RID_AT_DYNAMIC:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_at_dynamic_declaration (parser);
	  break;
	case RID_AT_END:
	  gcc_assert (c_dialect_objc ());
	  c_parser_consume_token (parser);
	  objc_finish_implementation ();
	  break;
	default:
	  goto decl_or_fndef;
	}
      break;
    case CPP_SEMICOLON:
      pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
	       "ISO C does not allow extra %<;%> outside of a function");
      c_parser_consume_token (parser);
      break;
    case CPP_PRAGMA:
      mark_valid_location_for_stdc_pragma (true);
      c_parser_pragma (parser, pragma_external);
      mark_valid_location_for_stdc_pragma (false);
      break;
    case CPP_PLUS:
    case CPP_MINUS:
      if (c_dialect_objc ())
	{
	  c_parser_objc_method_definition (parser);
	  break;
	}
      /* Else fall through, and yield a syntax error trying to parse
	 as a declaration or function definition.  */
    default:
    decl_or_fndef:
      /* A declaration or a function definition (or, in Objective-C,
	 an @interface or @protocol with prefix attributes).  We can
	 only tell which after parsing the declaration specifiers, if
	 any, and the first declarator.  */
      c_parser_declaration_or_fndef (parser, true, true, true, false, true,
				     NULL, vNULL);
      break;
    }
}

static void c_finish_omp_declare_simd (c_parser *, tree, tree, vec<c_token>);

/* Parse a declaration or function definition (C90 6.5, 6.7.1, C99
   6.7, 6.9.1).  If FNDEF_OK is true, a function definition is
   accepted; otherwise (old-style parameter declarations) only other
   declarations are accepted.  If STATIC_ASSERT_OK is true, a static
   assertion is accepted; otherwise (old-style parameter declarations)
   it is not.  If NESTED is true, we are inside a function or parsing
   old-style parameter declarations; any functions encountered are
   nested functions and declaration specifiers are required; otherwise
   we are at top level and functions are normal functions and
   declaration specifiers may be optional.  If EMPTY_OK is true, empty
   declarations are OK (subject to all other constraints); otherwise
   (old-style parameter declarations) they are diagnosed.  If
   START_ATTR_OK is true, the declaration specifiers may start with
   attributes; otherwise they may not.
   OBJC_FOREACH_OBJECT_DECLARATION can be used to get back the parsed
   declaration when parsing an Objective-C foreach statement.

   declaration:
     declaration-specifiers init-declarator-list[opt] ;
     static_assert-declaration

   function-definition:
     declaration-specifiers[opt] declarator declaration-list[opt]
       compound-statement

   declaration-list:
     declaration
     declaration-list declaration

   init-declarator-list:
     init-declarator
     init-declarator-list , init-declarator

   init-declarator:
     declarator simple-asm-expr[opt] attributes[opt]
     declarator simple-asm-expr[opt] attributes[opt] = initializer

   GNU extensions:

   nested-function-definition:
     declaration-specifiers declarator declaration-list[opt]
       compound-statement

   Objective-C:
     attributes objc-class-definition
     attributes objc-category-definition
     attributes objc-protocol-definition

   The simple-asm-expr and attributes are GNU extensions.

   This function does not handle __extension__; that is handled in its
   callers.  ??? Following the old parser, __extension__ may start
   external declarations, declarations in functions and declarations
   at the start of "for" loops, but not old-style parameter
   declarations.

   C99 requires declaration specifiers in a function definition; the
   absence is diagnosed through the diagnosis of implicit int.  In GNU
   C we also allow but diagnose declarations without declaration
   specifiers, but only at top level (elsewhere they conflict with
   other syntax).

   In Objective-C, declarations of the looping variable in a foreach
   statement are exceptionally terminated by 'in' (for example, 'for
   (NSObject *object in array) { ... }').

   OpenMP:

   declaration:
     threadprivate-directive  */

static void
c_parser_declaration_or_fndef (c_parser *parser, bool fndef_ok,
			       bool static_assert_ok, bool empty_ok,
			       bool nested, bool start_attr_ok,
			       tree *objc_foreach_object_declaration,
			       vec<c_token> omp_declare_simd_clauses)
{
  struct c_declspecs *specs;
  tree prefix_attrs;
  tree all_prefix_attrs;
  bool diagnosed_no_specs = false;
  location_t here = c_parser_peek_token (parser)->location;

  if (static_assert_ok
      && c_parser_next_token_is_keyword (parser, RID_STATIC_ASSERT))
    {
      c_parser_static_assert_declaration (parser);
      return;
    }
  specs = build_null_declspecs ();

  /* Try to detect an unknown type name when we have "A B" or "A *B".  */
  if (c_parser_peek_token (parser)->type == CPP_NAME
      && c_parser_peek_token (parser)->id_kind == C_ID_ID
      && (c_parser_peek_2nd_token (parser)->type == CPP_NAME
          || c_parser_peek_2nd_token (parser)->type == CPP_MULT)
      && (!nested || !lookup_name (c_parser_peek_token (parser)->value)))
    {
      error_at (here, "unknown type name %qE",
                c_parser_peek_token (parser)->value);

      /* Parse declspecs normally to get a correct pointer type, but avoid
         a further "fails to be a type name" error.  Refuse nested functions
         since it is not how the user likely wants us to recover.  */
      c_parser_peek_token (parser)->type = CPP_KEYWORD;
      c_parser_peek_token (parser)->keyword = RID_VOID;
      c_parser_peek_token (parser)->value = error_mark_node;
      fndef_ok = !nested;
    }

  c_parser_declspecs (parser, specs, true, true, start_attr_ok,
		      true, true, cla_nonabstract_decl);
  if (parser->error)
    {
      c_parser_skip_to_end_of_block_or_statement (parser);
      return;
    }
  if (nested && !specs->declspecs_seen_p)
    {
      c_parser_error (parser, "expected declaration specifiers");
      c_parser_skip_to_end_of_block_or_statement (parser);
      return;
    }
  finish_declspecs (specs);
  bool auto_type_p = specs->typespec_word == cts_auto_type;
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      if (auto_type_p)
	error_at (here, "%<__auto_type%> in empty declaration");
      else if (empty_ok)
	shadow_tag (specs);
      else
	{
	  shadow_tag_warned (specs, 1);
	  pedwarn (here, 0, "empty declaration");
	}
      c_parser_consume_token (parser);
      return;
    }

  /* Provide better error recovery.  Note that a type name here is usually
     better diagnosed as a redeclaration.  */
  if (empty_ok
      && specs->typespec_kind == ctsk_tagdef
      && c_parser_next_token_starts_declspecs (parser)
      && !c_parser_next_token_is (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected %<;%>, identifier or %<(%>");
      parser->error = false;
      shadow_tag_warned (specs, 1);
      return;
    }
  else if (c_dialect_objc () && !auto_type_p)
    {
      /* Prefix attributes are an error on method decls.  */
      switch (c_parser_peek_token (parser)->type)
	{
	  case CPP_PLUS:
	  case CPP_MINUS:
	    if (c_parser_objc_diagnose_bad_element_prefix (parser, specs))
	      return;
	    if (specs->attrs)
	      {
		warning_at (c_parser_peek_token (parser)->location, 
			    OPT_Wattributes,
	       		    "prefix attributes are ignored for methods");
		specs->attrs = NULL_TREE;
	      }
	    if (fndef_ok)
	      c_parser_objc_method_definition (parser);
	    else
	      c_parser_objc_methodproto (parser);
	    return;
	    break;
	  default:
	    break;
	}
      /* This is where we parse 'attributes @interface ...',
	 'attributes @implementation ...', 'attributes @protocol ...'
	 (where attributes could be, for example, __attribute__
	 ((deprecated)).
      */
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_AT_INTERFACE:
	  {
	    if (c_parser_objc_diagnose_bad_element_prefix (parser, specs))
	      return;
	    c_parser_objc_class_definition (parser, specs->attrs);
	    return;
	  }
	  break;
	case RID_AT_IMPLEMENTATION:
	  {
	    if (c_parser_objc_diagnose_bad_element_prefix (parser, specs))
	      return;
	    if (specs->attrs)
	      {
		warning_at (c_parser_peek_token (parser)->location, 
			OPT_Wattributes,
			"prefix attributes are ignored for implementations");
		specs->attrs = NULL_TREE;
	      }
	    c_parser_objc_class_definition (parser, NULL_TREE);	    
	    return;
	  }
	  break;
	case RID_AT_PROTOCOL:
	  {
	    if (c_parser_objc_diagnose_bad_element_prefix (parser, specs))
	      return;
	    c_parser_objc_protocol_definition (parser, specs->attrs);
	    return;
	  }
	  break;
	case RID_AT_ALIAS:
	case RID_AT_CLASS:
	case RID_AT_END:
	case RID_AT_PROPERTY:
	  if (specs->attrs)
	    {
	      c_parser_error (parser, "unexpected attribute");
	      specs->attrs = NULL;
	    }
	  break;
	default:
	  break;
	}
    }
  
  pending_xref_error ();
  prefix_attrs = specs->attrs;
  all_prefix_attrs = prefix_attrs;
  specs->attrs = NULL_TREE;
  while (true)
    {
      struct c_declarator *declarator;
      bool dummy = false;
      timevar_id_t tv;
      tree fnbody;
      /* Declaring either one or more declarators (in which case we
	 should diagnose if there were no declaration specifiers) or a
	 function definition (in which case the diagnostic for
	 implicit int suffices).  */
      declarator = c_parser_declarator (parser, 
					specs->typespec_kind != ctsk_none,
					C_DTR_NORMAL, &dummy);
      if (declarator == NULL)
	{
	  if (omp_declare_simd_clauses.exists ()
	      || !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
	    c_finish_omp_declare_simd (parser, NULL_TREE, NULL_TREE,
				       omp_declare_simd_clauses);
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      if (auto_type_p && declarator->kind != cdk_id)
	{
	  error_at (here,
		    "%<__auto_type%> requires a plain identifier"
		    " as declarator");
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      if (c_parser_next_token_is (parser, CPP_EQ)
	  || c_parser_next_token_is (parser, CPP_COMMA)
	  || c_parser_next_token_is (parser, CPP_SEMICOLON)
	  || c_parser_next_token_is_keyword (parser, RID_ASM)
	  || c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE)
	  || c_parser_next_token_is_keyword (parser, RID_IN))
	{
	  tree asm_name = NULL_TREE;
	  tree postfix_attrs = NULL_TREE;
	  if (!diagnosed_no_specs && !specs->declspecs_seen_p)
	    {
	      diagnosed_no_specs = true;
	      pedwarn (here, 0, "data definition has no type or storage class");
	    }
	  /* Having seen a data definition, there cannot now be a
	     function definition.  */
	  fndef_ok = false;
	  if (c_parser_next_token_is_keyword (parser, RID_ASM))
	    asm_name = c_parser_simple_asm_expr (parser);
	  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	    {
	      postfix_attrs = c_parser_attributes (parser);
	      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
		{
		  /* This means there is an attribute specifier after
		     the declarator in a function definition.  Provide
		     some more information for the user.  */
		  error_at (here, "attributes should be specified before the "
			    "declarator in a function definition");
		  c_parser_skip_to_end_of_block_or_statement (parser);
		  return;
		}
	    }
	  if (c_parser_next_token_is (parser, CPP_EQ))
	    {
	      tree d;
	      struct c_expr init;
	      location_t init_loc;
	      c_parser_consume_token (parser);
	      if (auto_type_p)
		{
		  start_init (NULL_TREE, asm_name, global_bindings_p ());
		  init_loc = c_parser_peek_token (parser)->location;
		  init = c_parser_expr_no_commas (parser, NULL);
		  if (TREE_CODE (init.value) == COMPONENT_REF
		      && DECL_C_BIT_FIELD (TREE_OPERAND (init.value, 1)))
		    error_at (here,
			      "%<__auto_type%> used with a bit-field"
			      " initializer");
		  init = convert_lvalue_to_rvalue (init_loc, init, true, true);
		  tree init_type = TREE_TYPE (init.value);
		  /* As with typeof, remove all qualifiers from atomic types.  */
		  if (init_type != error_mark_node && TYPE_ATOMIC (init_type))
		    init_type
		      = c_build_qualified_type (init_type, TYPE_UNQUALIFIED);
		  bool vm_type = variably_modified_type_p (init_type,
							   NULL_TREE);
		  if (vm_type)
		    init.value = c_save_expr (init.value);
		  finish_init ();
		  specs->typespec_kind = ctsk_typeof;
		  specs->locations[cdw_typedef] = init_loc;
		  specs->typedef_p = true;
		  specs->type = init_type;
		  if (vm_type)
		    {
		      bool maybe_const = true;
		      tree type_expr = c_fully_fold (init.value, false,
						     &maybe_const);
		      specs->expr_const_operands &= maybe_const;
		      if (specs->expr)
			specs->expr = build2 (COMPOUND_EXPR,
					      TREE_TYPE (type_expr),
					      specs->expr, type_expr);
		      else
			specs->expr = type_expr;
		    }
		  d = start_decl (declarator, specs, true,
				  chainon (postfix_attrs, all_prefix_attrs));
		  if (!d)
		    d = error_mark_node;
		  if (omp_declare_simd_clauses.exists ()
		      || !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
		    c_finish_omp_declare_simd (parser, d, NULL_TREE,
					       omp_declare_simd_clauses);
		}
	      else
		{
		  /* The declaration of the variable is in effect while
		     its initializer is parsed.  */
		  d = start_decl (declarator, specs, true,
				  chainon (postfix_attrs, all_prefix_attrs));
		  if (!d)
		    d = error_mark_node;
		  if (omp_declare_simd_clauses.exists ()
		      || !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
		    c_finish_omp_declare_simd (parser, d, NULL_TREE,
					       omp_declare_simd_clauses);
		  start_init (d, asm_name, global_bindings_p ());
		  init_loc = c_parser_peek_token (parser)->location;
		  init = c_parser_initializer (parser);
		  finish_init ();
		}
	      if (d != error_mark_node)
		{
		  maybe_warn_string_init (init_loc, TREE_TYPE (d), init);
		  finish_decl (d, init_loc, init.value,
			       init.original_type, asm_name);
		}
	    }
	  else
	    {
	      if (auto_type_p)
		{
		  error_at (here,
			    "%<__auto_type%> requires an initialized "
			    "data declaration");
		  c_parser_skip_to_end_of_block_or_statement (parser);
		  return;
		}
	      tree d = start_decl (declarator, specs, false,
				   chainon (postfix_attrs,
					    all_prefix_attrs));
	      if (omp_declare_simd_clauses.exists ()
		  || !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
		{
		  tree parms = NULL_TREE;
		  if (d && TREE_CODE (d) == FUNCTION_DECL)
		    {
		      struct c_declarator *ce = declarator;
		      while (ce != NULL)
			if (ce->kind == cdk_function)
			  {
			    parms = ce->u.arg_info->parms;
			    break;
			  }
			else
			  ce = ce->declarator;
		    }
		  if (parms)
		    temp_store_parm_decls (d, parms);
		  c_finish_omp_declare_simd (parser, d, parms,
					     omp_declare_simd_clauses);
		  if (parms)
		    temp_pop_parm_decls ();
		}
	      if (d)
		finish_decl (d, UNKNOWN_LOCATION, NULL_TREE,
			     NULL_TREE, asm_name);
	      
	      if (c_parser_next_token_is_keyword (parser, RID_IN))
		{
		  if (d)
		    *objc_foreach_object_declaration = d;
		  else
		    *objc_foreach_object_declaration = error_mark_node;		    
		}
	    }
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    {
	      if (auto_type_p)
		{
		  error_at (here,
			    "%<__auto_type%> may only be used with"
			    " a single declarator");
		  c_parser_skip_to_end_of_block_or_statement (parser);
		  return;
		}
	      c_parser_consume_token (parser);
	      if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
		all_prefix_attrs = chainon (c_parser_attributes (parser),
					    prefix_attrs);
	      else
		all_prefix_attrs = prefix_attrs;
	      continue;
	    }
	  else if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      c_parser_consume_token (parser);
	      return;
	    }
	  else if (c_parser_next_token_is_keyword (parser, RID_IN))
	    {
	      /* This can only happen in Objective-C: we found the
		 'in' that terminates the declaration inside an
		 Objective-C foreach statement.  Do not consume the
		 token, so that the caller can use it to determine
		 that this indeed is a foreach context.  */
	      return;
	    }
	  else
	    {
	      c_parser_error (parser, "expected %<,%> or %<;%>");
	      c_parser_skip_to_end_of_block_or_statement (parser);
	      return;
	    }
	}
      else if (auto_type_p)
	{
	  error_at (here,
		    "%<__auto_type%> requires an initialized data declaration");
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      else if (!fndef_ok)
	{
	  c_parser_error (parser, "expected %<=%>, %<,%>, %<;%>, "
			  "%<asm%> or %<__attribute__%>");
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      /* Function definition (nested or otherwise).  */
      if (nested)
	{
	  pedwarn (here, OPT_Wpedantic, "ISO C forbids nested functions");
	  c_push_function_context ();
	}
      if (!start_function (specs, declarator, all_prefix_attrs))
	{
	  /* This can appear in many cases looking nothing like a
	     function definition, so we don't give a more specific
	     error suggesting there was one.  */
	  c_parser_error (parser, "expected %<=%>, %<,%>, %<;%>, %<asm%> "
			  "or %<__attribute__%>");
	  if (nested)
	    c_pop_function_context ();
	  break;
	}

      if (DECL_DECLARED_INLINE_P (current_function_decl))
        tv = TV_PARSE_INLINE;
      else
        tv = TV_PARSE_FUNC;
      timevar_push (tv);

      /* Parse old-style parameter declarations.  ??? Attributes are
	 not allowed to start declaration specifiers here because of a
	 syntax conflict between a function declaration with attribute
	 suffix and a function definition with an attribute prefix on
	 first old-style parameter declaration.  Following the old
	 parser, they are not accepted on subsequent old-style
	 parameter declarations either.  However, there is no
	 ambiguity after the first declaration, nor indeed on the
	 first as long as we don't allow postfix attributes after a
	 declarator with a nonempty identifier list in a definition;
	 and postfix attributes have never been accepted here in
	 function definitions either.  */
      while (c_parser_next_token_is_not (parser, CPP_EOF)
	     && c_parser_next_token_is_not (parser, CPP_OPEN_BRACE))
	c_parser_declaration_or_fndef (parser, false, false, false,
				       true, false, NULL, vNULL);
      store_parm_decls ();
      if (omp_declare_simd_clauses.exists ()
	  || !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
	c_finish_omp_declare_simd (parser, current_function_decl, NULL_TREE,
				   omp_declare_simd_clauses);
      DECL_STRUCT_FUNCTION (current_function_decl)->function_start_locus
	= c_parser_peek_token (parser)->location;
      fnbody = c_parser_compound_statement (parser);
      if (flag_cilkplus && contains_array_notation_expr (fnbody))
	fnbody = expand_array_notation_exprs (fnbody);
      if (nested)
	{
	  tree decl = current_function_decl;
	  /* Mark nested functions as needing static-chain initially.
	     lower_nested_functions will recompute it but the
	     DECL_STATIC_CHAIN flag is also used before that happens,
	     by initializer_constant_valid_p.  See gcc.dg/nested-fn-2.c.  */
	  DECL_STATIC_CHAIN (decl) = 1;
	  add_stmt (fnbody);
	  finish_function ();
	  c_pop_function_context ();
	  add_stmt (build_stmt (DECL_SOURCE_LOCATION (decl), DECL_EXPR, decl));
	}
      else
	{
	  add_stmt (fnbody);
	  finish_function ();
	}

      timevar_pop (tv);
      break;
    }
}

/* Parse an asm-definition (asm() outside a function body).  This is a
   GNU extension.

   asm-definition:
     simple-asm-expr ;
*/

static void
c_parser_asm_definition (c_parser *parser)
{
  tree asm_str = c_parser_simple_asm_expr (parser);
  if (asm_str)
    symtab->finalize_toplevel_asm (asm_str);
  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
}

/* Parse a static assertion (C11 6.7.10).

   static_assert-declaration:
     static_assert-declaration-no-semi ;
*/

static void
c_parser_static_assert_declaration (c_parser *parser)
{
  c_parser_static_assert_declaration_no_semi (parser);
  if (parser->error
      || !c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>"))
    c_parser_skip_to_end_of_block_or_statement (parser);
}

/* Parse a static assertion (C11 6.7.10), without the trailing
   semicolon.

   static_assert-declaration-no-semi:
     _Static_assert ( constant-expression , string-literal )
*/

static void
c_parser_static_assert_declaration_no_semi (c_parser *parser)
{
  location_t assert_loc, value_loc;
  tree value;
  tree string;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_STATIC_ASSERT));
  assert_loc = c_parser_peek_token (parser)->location;
  if (flag_isoc99)
    pedwarn_c99 (assert_loc, OPT_Wpedantic,
		 "ISO C99 does not support %<_Static_assert%>");
  else
    pedwarn_c99 (assert_loc, OPT_Wpedantic,
		 "ISO C90 does not support %<_Static_assert%>");
  c_parser_consume_token (parser);
  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return;
  value_loc = c_parser_peek_token (parser)->location;
  value = c_parser_expr_no_commas (parser, NULL).value;
  parser->lex_untranslated_string = true;
  if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
    {
      parser->lex_untranslated_string = false;
      return;
    }
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_STRING:
    case CPP_STRING16:
    case CPP_STRING32:
    case CPP_WSTRING:
    case CPP_UTF8STRING:
      string = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
      parser->lex_untranslated_string = false;
      break;
    default:
      c_parser_error (parser, "expected string literal");
      parser->lex_untranslated_string = false;
      return;
    }
  c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>");

  if (!INTEGRAL_TYPE_P (TREE_TYPE (value)))
    {
      error_at (value_loc, "expression in static assertion is not an integer");
      return;
    }
  if (TREE_CODE (value) != INTEGER_CST)
    {
      value = c_fully_fold (value, false, NULL);
      /* Strip no-op conversions.  */
      STRIP_TYPE_NOPS (value);
      if (TREE_CODE (value) == INTEGER_CST)
	pedwarn (value_loc, OPT_Wpedantic, "expression in static assertion "
		 "is not an integer constant expression");
    }
  if (TREE_CODE (value) != INTEGER_CST)
    {
      error_at (value_loc, "expression in static assertion is not constant");
      return;
    }
  constant_expression_warning (value);
  if (integer_zerop (value))
    error_at (assert_loc, "static assertion failed: %E", string);
}

/* Parse some declaration specifiers (possibly none) (C90 6.5, C99
   6.7), adding them to SPECS (which may already include some).
   Storage class specifiers are accepted iff SCSPEC_OK; type
   specifiers are accepted iff TYPESPEC_OK; alignment specifiers are
   accepted iff ALIGNSPEC_OK; attributes are accepted at the start
   iff START_ATTR_OK; __auto_type is accepted iff AUTO_TYPE_OK.

   declaration-specifiers:
     storage-class-specifier declaration-specifiers[opt]
     type-specifier declaration-specifiers[opt]
     type-qualifier declaration-specifiers[opt]
     function-specifier declaration-specifiers[opt]
     alignment-specifier declaration-specifiers[opt]

   Function specifiers (inline) are from C99, and are currently
   handled as storage class specifiers, as is __thread.  Alignment
   specifiers are from C11.

   C90 6.5.1, C99 6.7.1:
   storage-class-specifier:
     typedef
     extern
     static
     auto
     register
     _Thread_local

   (_Thread_local is new in C11.)

   C99 6.7.4:
   function-specifier:
     inline
     _Noreturn

   (_Noreturn is new in C11.)

   C90 6.5.2, C99 6.7.2:
   type-specifier:
     void
     char
     short
     int
     long
     float
     double
     signed
     unsigned
     _Bool
     _Complex
     [_Imaginary removed in C99 TC2]
     struct-or-union-specifier
     enum-specifier
     typedef-name
     atomic-type-specifier

   (_Bool and _Complex are new in C99.)
   (atomic-type-specifier is new in C11.)

   C90 6.5.3, C99 6.7.3:

   type-qualifier:
     const
     restrict
     volatile
     address-space-qualifier
     _Atomic

   (restrict is new in C99.)
   (_Atomic is new in C11.)

   GNU extensions:

   declaration-specifiers:
     attributes declaration-specifiers[opt]

   type-qualifier:
     address-space

   address-space:
     identifier recognized by the target

   storage-class-specifier:
     __thread

   type-specifier:
     typeof-specifier
     __auto_type
     __intN
     _Decimal32
     _Decimal64
     _Decimal128
     _Fract
     _Accum
     _Sat

  (_Fract, _Accum, and _Sat are new from ISO/IEC DTR 18037:
   http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1169.pdf)

   atomic-type-specifier
    _Atomic ( type-name )

   Objective-C:

   type-specifier:
     class-name objc-protocol-refs[opt]
     typedef-name objc-protocol-refs
     objc-protocol-refs
*/

static void
c_parser_declspecs (c_parser *parser, struct c_declspecs *specs,
		    bool scspec_ok, bool typespec_ok, bool start_attr_ok,
		    bool alignspec_ok, bool auto_type_ok,
		    enum c_lookahead_kind la)
{
  bool attrs_ok = start_attr_ok;
  bool seen_type = specs->typespec_kind != ctsk_none;

  if (!typespec_ok)
    gcc_assert (la == cla_prefer_id);

  while (c_parser_next_token_is (parser, CPP_NAME)
	 || c_parser_next_token_is (parser, CPP_KEYWORD)
	 || (c_dialect_objc () && c_parser_next_token_is (parser, CPP_LESS)))
    {
      struct c_typespec t;
      tree attrs;
      tree align;
      location_t loc = c_parser_peek_token (parser)->location;

      /* If we cannot accept a type, exit if the next token must start
	 one.  Also, if we already have seen a tagged definition,
	 a typename would be an error anyway and likely the user
	 has simply forgotten a semicolon, so we exit.  */
      if ((!typespec_ok || specs->typespec_kind == ctsk_tagdef)
	  && c_parser_next_tokens_start_typename (parser, la)
	  && !c_parser_next_token_is_qualifier (parser))
	break;

      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  c_token *name_token = c_parser_peek_token (parser);
	  tree value = name_token->value;
	  c_id_kind kind = name_token->id_kind;

	  if (kind == C_ID_ADDRSPACE)
	    {
	      addr_space_t as
		= name_token->keyword - RID_FIRST_ADDR_SPACE;
	      declspecs_add_addrspace (name_token->location, specs, as);
	      c_parser_consume_token (parser);
	      attrs_ok = true;
	      continue;
	    }

	  gcc_assert (!c_parser_next_token_is_qualifier (parser));

	  /* If we cannot accept a type, and the next token must start one,
	     exit.  Do the same if we already have seen a tagged definition,
	     since it would be an error anyway and likely the user has simply
	     forgotten a semicolon.  */
	  if (seen_type || !c_parser_next_tokens_start_typename (parser, la))
	    break;

	  /* Now at an unknown typename (C_ID_ID), a C_ID_TYPENAME or
	     a C_ID_CLASSNAME.  */
	  c_parser_consume_token (parser);
	  seen_type = true;
	  attrs_ok = true;
	  if (kind == C_ID_ID)
	    {
	      error_at (loc, "unknown type name %qE", value);
	      t.kind = ctsk_typedef;
	      t.spec = error_mark_node;
	    }
	  else if (kind == C_ID_TYPENAME
	           && (!c_dialect_objc ()
	               || c_parser_next_token_is_not (parser, CPP_LESS)))
	    {
	      t.kind = ctsk_typedef;
	      /* For a typedef name, record the meaning, not the name.
		 In case of 'foo foo, bar;'.  */
	      t.spec = lookup_name (value);
	    }
	  else
	    {
	      tree proto = NULL_TREE;
	      gcc_assert (c_dialect_objc ());
	      t.kind = ctsk_objc;
	      if (c_parser_next_token_is (parser, CPP_LESS))
		proto = c_parser_objc_protocol_refs (parser);
	      t.spec = objc_get_protocol_qualified_type (value, proto);
	    }
	  t.expr = NULL_TREE;
	  t.expr_const_operands = true;
	  declspecs_add_type (name_token->location, specs, t);
	  continue;
	}
      if (c_parser_next_token_is (parser, CPP_LESS))
	{
	  /* Make "<SomeProtocol>" equivalent to "id <SomeProtocol>" -
	     nisse@lysator.liu.se.  */
	  tree proto;
	  gcc_assert (c_dialect_objc ());
	  if (!typespec_ok || seen_type)
	    break;
	  proto = c_parser_objc_protocol_refs (parser);
	  t.kind = ctsk_objc;
	  t.spec = objc_get_protocol_qualified_type (NULL_TREE, proto);
	  t.expr = NULL_TREE;
	  t.expr_const_operands = true;
	  declspecs_add_type (loc, specs, t);
	  continue;
	}
      gcc_assert (c_parser_next_token_is (parser, CPP_KEYWORD));
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_STATIC:
	case RID_EXTERN:
	case RID_REGISTER:
	case RID_TYPEDEF:
	case RID_INLINE:
	case RID_NORETURN:
	case RID_AUTO:
	case RID_THREAD:
	  if (!scspec_ok)
	    goto out;
	  attrs_ok = true;
	  /* TODO: Distinguish between function specifiers (inline, noreturn)
	     and storage class specifiers, either here or in
	     declspecs_add_scspec.  */
	  declspecs_add_scspec (loc, specs,
				c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	case RID_AUTO_TYPE:
	  if (!auto_type_ok)
	    goto out;
	  /* Fall through.  */
	case RID_UNSIGNED:
	case RID_LONG:
	case RID_SHORT:
	case RID_SIGNED:
	case RID_COMPLEX:
	case RID_INT:
	case RID_CHAR:
	case RID_FLOAT:
	case RID_DOUBLE:
	case RID_VOID:
	case RID_DFLOAT32:
	case RID_DFLOAT64:
	case RID_DFLOAT128:
	case RID_BOOL:
	case RID_FRACT:
	case RID_ACCUM:
	case RID_SAT:
	case RID_INT_N_0:
	case RID_INT_N_1:
	case RID_INT_N_2:
	case RID_INT_N_3:
	  if (!typespec_ok)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  if (c_dialect_objc ())
	    parser->objc_need_raw_identifier = true;
	  t.kind = ctsk_resword;
	  t.spec = c_parser_peek_token (parser)->value;
	  t.expr = NULL_TREE;
	  t.expr_const_operands = true;
	  declspecs_add_type (loc, specs, t);
	  c_parser_consume_token (parser);
	  break;
	case RID_ENUM:
	  if (!typespec_ok)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  t = c_parser_enum_specifier (parser);
	  declspecs_add_type (loc, specs, t);
	  break;
	case RID_STRUCT:
	case RID_UNION:
	  if (!typespec_ok)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  t = c_parser_struct_or_union_specifier (parser);
          invoke_plugin_callbacks (PLUGIN_FINISH_TYPE, t.spec);
	  declspecs_add_type (loc, specs, t);
	  break;
	case RID_TYPEOF:
	  /* ??? The old parser rejected typeof after other type
	     specifiers, but is a syntax error the best way of
	     handling this?  */
	  if (!typespec_ok || seen_type)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  t = c_parser_typeof_specifier (parser);
	  declspecs_add_type (loc, specs, t);
	  break;
	case RID_ATOMIC:
	  /* C parser handling of Objective-C constructs needs
	     checking for correct lvalue-to-rvalue conversions, and
	     the code in build_modify_expr handling various
	     Objective-C cases, and that in build_unary_op handling
	     Objective-C cases for increment / decrement, also needs
	     updating; uses of TYPE_MAIN_VARIANT in objc_compare_types
	     and objc_types_are_equivalent may also need updates.  */
	  if (c_dialect_objc ())
	    sorry ("%<_Atomic%> in Objective-C");
	  /* C parser handling of OpenMP constructs needs checking for
	     correct lvalue-to-rvalue conversions.  */
	  if (flag_openmp)
	    sorry ("%<_Atomic%> with OpenMP");
	  if (flag_isoc99)
	    pedwarn_c99 (loc, OPT_Wpedantic,
			 "ISO C99 does not support the %<_Atomic%> qualifier");
	  else
	    pedwarn_c99 (loc, OPT_Wpedantic,
			 "ISO C90 does not support the %<_Atomic%> qualifier");
	  attrs_ok = true;
	  tree value;
	  value = c_parser_peek_token (parser)->value;
	  c_parser_consume_token (parser);
	  if (typespec_ok && c_parser_next_token_is (parser, CPP_OPEN_PAREN))
	    {
	      /* _Atomic ( type-name ).  */
	      seen_type = true;
	      c_parser_consume_token (parser);
	      struct c_type_name *type = c_parser_type_name (parser);
	      t.kind = ctsk_typeof;
	      t.spec = error_mark_node;
	      t.expr = NULL_TREE;
	      t.expr_const_operands = true;
	      if (type != NULL)
		t.spec = groktypename (type, &t.expr,
				       &t.expr_const_operands);
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
					 "expected %<)%>");
	      if (t.spec != error_mark_node)
		{
		  if (TREE_CODE (t.spec) == ARRAY_TYPE)
		    error_at (loc, "%<_Atomic%>-qualified array type");
		  else if (TREE_CODE (t.spec) == FUNCTION_TYPE)
		    error_at (loc, "%<_Atomic%>-qualified function type");
		  else if (TYPE_QUALS (t.spec) != TYPE_UNQUALIFIED)
		    error_at (loc, "%<_Atomic%> applied to a qualified type");
		  else
		    t.spec = c_build_qualified_type (t.spec, TYPE_QUAL_ATOMIC);
		}
	      declspecs_add_type (loc, specs, t);
	    }
	  else
	    declspecs_add_qual (loc, specs, value);
	  break;
	case RID_CONST:
	case RID_VOLATILE:
	case RID_RESTRICT:
	  attrs_ok = true;
	  declspecs_add_qual (loc, specs, c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	case RID_ATTRIBUTE:
	  if (!attrs_ok)
	    goto out;
	  attrs = c_parser_attributes (parser);
	  declspecs_add_attrs (loc, specs, attrs);
	  break;
	case RID_ALIGNAS:
	  if (!alignspec_ok)
	    goto out;
	  align = c_parser_alignas_specifier (parser);
	  declspecs_add_alignas (loc, specs, align);
	  break;
	default:
	  goto out;
	}
    }
 out: ;
}

/* Parse an enum specifier (C90 6.5.2.2, C99 6.7.2.2).

   enum-specifier:
     enum attributes[opt] identifier[opt] { enumerator-list } attributes[opt]
     enum attributes[opt] identifier[opt] { enumerator-list , } attributes[opt]
     enum attributes[opt] identifier

   The form with trailing comma is new in C99.  The forms with
   attributes are GNU extensions.  In GNU C, we accept any expression
   without commas in the syntax (assignment expressions, not just
   conditional expressions); assignment expressions will be diagnosed
   as non-constant.

   enumerator-list:
     enumerator
     enumerator-list , enumerator

   enumerator:
     enumeration-constant
     enumeration-constant = constant-expression
*/

static struct c_typespec
c_parser_enum_specifier (c_parser *parser)
{
  struct c_typespec ret;
  tree attrs;
  tree ident = NULL_TREE;
  location_t enum_loc;
  location_t ident_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ENUM));
  enum_loc = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);
  attrs = c_parser_attributes (parser);
  enum_loc = c_parser_peek_token (parser)->location;
  /* Set the location in case we create a decl now.  */
  c_parser_set_source_position_from_token (c_parser_peek_token (parser));
  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      ident = c_parser_peek_token (parser)->value;
      ident_loc = c_parser_peek_token (parser)->location;
      enum_loc = ident_loc;
      c_parser_consume_token (parser);
    }
  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    {
      /* Parse an enum definition.  */
      struct c_enum_contents the_enum;
      tree type;
      tree postfix_attrs;
      /* We chain the enumerators in reverse order, then put them in
	 forward order at the end.  */
      tree values;
      timevar_push (TV_PARSE_ENUM);
      type = start_enum (enum_loc, &the_enum, ident);
      values = NULL_TREE;
      c_parser_consume_token (parser);
      while (true)
	{
	  tree enum_id;
	  tree enum_value;
	  tree enum_decl;
	  bool seen_comma;
	  c_token *token;
	  location_t comma_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
	  location_t decl_loc, value_loc;
	  if (c_parser_next_token_is_not (parser, CPP_NAME))
	    {
	      c_parser_error (parser, "expected identifier");
	      c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
	      values = error_mark_node;
	      break;
	    }
	  token = c_parser_peek_token (parser);
	  enum_id = token->value;
	  /* Set the location in case we create a decl now.  */
	  c_parser_set_source_position_from_token (token);
	  decl_loc = value_loc = token->location;
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_EQ))
	    {
	      c_parser_consume_token (parser);
	      value_loc = c_parser_peek_token (parser)->location;
	      enum_value = c_parser_expr_no_commas (parser, NULL).value;
	    }
	  else
	    enum_value = NULL_TREE;
	  enum_decl = build_enumerator (decl_loc, value_loc,
	      				&the_enum, enum_id, enum_value);
	  TREE_CHAIN (enum_decl) = values;
	  values = enum_decl;
	  seen_comma = false;
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    {
	      comma_loc = c_parser_peek_token (parser)->location;
	      seen_comma = true;
	      c_parser_consume_token (parser);
	    }
	  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    {
	      if (seen_comma)
		pedwarn_c90 (comma_loc, OPT_Wpedantic,
			     "comma at end of enumerator list");
	      c_parser_consume_token (parser);
	      break;
	    }
	  if (!seen_comma)
	    {
	      c_parser_error (parser, "expected %<,%> or %<}%>");
	      c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
	      values = error_mark_node;
	      break;
	    }
	}
      postfix_attrs = c_parser_attributes (parser);
      ret.spec = finish_enum (type, nreverse (values),
			      chainon (attrs, postfix_attrs));
      ret.kind = ctsk_tagdef;
      ret.expr = NULL_TREE;
      ret.expr_const_operands = true;
      timevar_pop (TV_PARSE_ENUM);
      return ret;
    }
  else if (!ident)
    {
      c_parser_error (parser, "expected %<{%>");
      ret.spec = error_mark_node;
      ret.kind = ctsk_tagref;
      ret.expr = NULL_TREE;
      ret.expr_const_operands = true;
      return ret;
    }
  ret = parser_xref_tag (ident_loc, ENUMERAL_TYPE, ident);
  /* In ISO C, enumerated types can be referred to only if already
     defined.  */
  if (pedantic && !COMPLETE_TYPE_P (ret.spec))
    {
      gcc_assert (ident);
      pedwarn (enum_loc, OPT_Wpedantic,
	       "ISO C forbids forward references to %<enum%> types");
    }
  return ret;
}

/* Parse a struct or union specifier (C90 6.5.2.1, C99 6.7.2.1).

   struct-or-union-specifier:
     struct-or-union attributes[opt] identifier[opt]
       { struct-contents } attributes[opt]
     struct-or-union attributes[opt] identifier

   struct-contents:
     struct-declaration-list

   struct-declaration-list:
     struct-declaration ;
     struct-declaration-list struct-declaration ;

   GNU extensions:

   struct-contents:
     empty
     struct-declaration
     struct-declaration-list struct-declaration

   struct-declaration-list:
     struct-declaration-list ;
     ;

   (Note that in the syntax here, unlike that in ISO C, the semicolons
   are included here rather than in struct-declaration, in order to
   describe the syntax with extra semicolons and missing semicolon at
   end.)

   Objective-C:

   struct-declaration-list:
     @defs ( class-name )

   (Note this does not include a trailing semicolon, but can be
   followed by further declarations, and gets a pedwarn-if-pedantic
   when followed by a semicolon.)  */

static struct c_typespec
c_parser_struct_or_union_specifier (c_parser *parser)
{
  struct c_typespec ret;
  tree attrs;
  tree ident = NULL_TREE;
  location_t struct_loc;
  location_t ident_loc = UNKNOWN_LOCATION;
  enum tree_code code;
  switch (c_parser_peek_token (parser)->keyword)
    {
    case RID_STRUCT:
      code = RECORD_TYPE;
      break;
    case RID_UNION:
      code = UNION_TYPE;
      break;
    default:
      gcc_unreachable ();
    }
  struct_loc = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);
  attrs = c_parser_attributes (parser);

  /* Set the location in case we create a decl now.  */
  c_parser_set_source_position_from_token (c_parser_peek_token (parser));

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      ident = c_parser_peek_token (parser)->value;
      ident_loc = c_parser_peek_token (parser)->location;
      struct_loc = ident_loc;
      c_parser_consume_token (parser);
    }
  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    {
      /* Parse a struct or union definition.  Start the scope of the
	 tag before parsing components.  */
      struct c_struct_parse_info *struct_info;
      tree type = start_struct (struct_loc, code, ident, &struct_info);
      tree postfix_attrs;
      /* We chain the components in reverse order, then put them in
	 forward order at the end.  Each struct-declaration may
	 declare multiple components (comma-separated), so we must use
	 chainon to join them, although when parsing each
	 struct-declaration we can use TREE_CHAIN directly.

	 The theory behind all this is that there will be more
	 semicolon separated fields than comma separated fields, and
	 so we'll be minimizing the number of node traversals required
	 by chainon.  */
      tree contents;
      timevar_push (TV_PARSE_STRUCT);
      contents = NULL_TREE;
      c_parser_consume_token (parser);
      /* Handle the Objective-C @defs construct,
	 e.g. foo(sizeof(struct{ @defs(ClassName) }));.  */
      if (c_parser_next_token_is_keyword (parser, RID_AT_DEFS))
	{
	  tree name;
	  gcc_assert (c_dialect_objc ());
	  c_parser_consume_token (parser);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    goto end_at_defs;
	  if (c_parser_next_token_is (parser, CPP_NAME)
	      && c_parser_peek_token (parser)->id_kind == C_ID_CLASSNAME)
	    {
	      name = c_parser_peek_token (parser)->value;
	      c_parser_consume_token (parser);
	    }
	  else
	    {
	      c_parser_error (parser, "expected class name");
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      goto end_at_defs;
	    }
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  contents = nreverse (objc_get_class_ivars (name));
	}
    end_at_defs:
      /* Parse the struct-declarations and semicolons.  Problems with
	 semicolons are diagnosed here; empty structures are diagnosed
	 elsewhere.  */
      while (true)
	{
	  tree decls;
	  /* Parse any stray semicolon.  */
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
		       "extra semicolon in struct or union specified");
	      c_parser_consume_token (parser);
	      continue;
	    }
	  /* Stop if at the end of the struct or union contents.  */
	  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    {
	      c_parser_consume_token (parser);
	      break;
	    }
	  /* Accept #pragmas at struct scope.  */
	  if (c_parser_next_token_is (parser, CPP_PRAGMA))
	    {
	      c_parser_pragma (parser, pragma_struct);
	      continue;
	    }
	  /* Parse some comma-separated declarations, but not the
	     trailing semicolon if any.  */
	  decls = c_parser_struct_declaration (parser);
	  contents = chainon (decls, contents);
	  /* If no semicolon follows, either we have a parse error or
	     are at the end of the struct or union and should
	     pedwarn.  */
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    c_parser_consume_token (parser);
	  else
	    {
	      if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
		pedwarn (c_parser_peek_token (parser)->location, 0,
			 "no semicolon at end of struct or union");
	      else if (parser->error
		       || !c_parser_next_token_starts_declspecs (parser))
		{
		  c_parser_error (parser, "expected %<;%>");
		  c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
		  break;
		}

	      /* If we come here, we have already emitted an error
		 for an expected `;', identifier or `(', and we also
	         recovered already.  Go on with the next field. */
	    }
	}
      postfix_attrs = c_parser_attributes (parser);
      ret.spec = finish_struct (struct_loc, type, nreverse (contents),
				chainon (attrs, postfix_attrs), struct_info);
      ret.kind = ctsk_tagdef;
      ret.expr = NULL_TREE;
      ret.expr_const_operands = true;
      timevar_pop (TV_PARSE_STRUCT);
      return ret;
    }
  else if (!ident)
    {
      c_parser_error (parser, "expected %<{%>");
      ret.spec = error_mark_node;
      ret.kind = ctsk_tagref;
      ret.expr = NULL_TREE;
      ret.expr_const_operands = true;
      return ret;
    }
  ret = parser_xref_tag (ident_loc, code, ident);
  return ret;
}

/* Parse a struct-declaration (C90 6.5.2.1, C99 6.7.2.1), *without*
   the trailing semicolon.

   struct-declaration:
     specifier-qualifier-list struct-declarator-list
     static_assert-declaration-no-semi

   specifier-qualifier-list:
     type-specifier specifier-qualifier-list[opt]
     type-qualifier specifier-qualifier-list[opt]
     attributes specifier-qualifier-list[opt]

   struct-declarator-list:
     struct-declarator
     struct-declarator-list , attributes[opt] struct-declarator

   struct-declarator:
     declarator attributes[opt]
     declarator[opt] : constant-expression attributes[opt]

   GNU extensions:

   struct-declaration:
     __extension__ struct-declaration
     specifier-qualifier-list

   Unlike the ISO C syntax, semicolons are handled elsewhere.  The use
   of attributes where shown is a GNU extension.  In GNU C, we accept
   any expression without commas in the syntax (assignment
   expressions, not just conditional expressions); assignment
   expressions will be diagnosed as non-constant.  */

static tree
c_parser_struct_declaration (c_parser *parser)
{
  struct c_declspecs *specs;
  tree prefix_attrs;
  tree all_prefix_attrs;
  tree decls;
  location_t decl_loc;
  if (c_parser_next_token_is_keyword (parser, RID_EXTENSION))
    {
      int ext;
      tree decl;
      ext = disable_extension_diagnostics ();
      c_parser_consume_token (parser);
      decl = c_parser_struct_declaration (parser);
      restore_extension_diagnostics (ext);
      return decl;
    }
  if (c_parser_next_token_is_keyword (parser, RID_STATIC_ASSERT))
    {
      c_parser_static_assert_declaration_no_semi (parser);
      return NULL_TREE;
    }
  specs = build_null_declspecs ();
  decl_loc = c_parser_peek_token (parser)->location;
  /* Strictly by the standard, we shouldn't allow _Alignas here,
     but it appears to have been intended to allow it there, so
     we're keeping it as it is until WG14 reaches a conclusion
     of N1731.
     <http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1731.pdf>  */
  c_parser_declspecs (parser, specs, false, true, true,
		      true, false, cla_nonabstract_decl);
  if (parser->error)
    return NULL_TREE;
  if (!specs->declspecs_seen_p)
    {
      c_parser_error (parser, "expected specifier-qualifier-list");
      return NULL_TREE;
    }
  finish_declspecs (specs);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON)
      || c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      tree ret;
      if (specs->typespec_kind == ctsk_none)
	{
	  pedwarn (decl_loc, OPT_Wpedantic,
		   "ISO C forbids member declarations with no members");
	  shadow_tag_warned (specs, pedantic);
	  ret = NULL_TREE;
	}
      else
	{
	  /* Support for unnamed structs or unions as members of
	     structs or unions (which is [a] useful and [b] supports
	     MS P-SDK).  */
	  tree attrs = NULL;

	  ret = grokfield (c_parser_peek_token (parser)->location,
			   build_id_declarator (NULL_TREE), specs,
			   NULL_TREE, &attrs);
	  if (ret)
	    decl_attributes (&ret, attrs, 0);
	}
      return ret;
    }

  /* Provide better error recovery.  Note that a type name here is valid,
     and will be treated as a field name.  */
  if (specs->typespec_kind == ctsk_tagdef
      && TREE_CODE (specs->type) != ENUMERAL_TYPE
      && c_parser_next_token_starts_declspecs (parser)
      && !c_parser_next_token_is (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected %<;%>, identifier or %<(%>");
      parser->error = false;
      return NULL_TREE;
    }

  pending_xref_error ();
  prefix_attrs = specs->attrs;
  all_prefix_attrs = prefix_attrs;
  specs->attrs = NULL_TREE;
  decls = NULL_TREE;
  while (true)
    {
      /* Declaring one or more declarators or un-named bit-fields.  */
      struct c_declarator *declarator;
      bool dummy = false;
      if (c_parser_next_token_is (parser, CPP_COLON))
	declarator = build_id_declarator (NULL_TREE);
      else
	declarator = c_parser_declarator (parser,
					  specs->typespec_kind != ctsk_none,
					  C_DTR_NORMAL, &dummy);
      if (declarator == NULL)
	{
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  break;
	}
      if (c_parser_next_token_is (parser, CPP_COLON)
	  || c_parser_next_token_is (parser, CPP_COMMA)
	  || c_parser_next_token_is (parser, CPP_SEMICOLON)
	  || c_parser_next_token_is (parser, CPP_CLOSE_BRACE)
	  || c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	{
	  tree postfix_attrs = NULL_TREE;
	  tree width = NULL_TREE;
	  tree d;
	  if (c_parser_next_token_is (parser, CPP_COLON))
	    {
	      c_parser_consume_token (parser);
	      width = c_parser_expr_no_commas (parser, NULL).value;
	    }
	  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	    postfix_attrs = c_parser_attributes (parser);
	  d = grokfield (c_parser_peek_token (parser)->location,
			 declarator, specs, width, &all_prefix_attrs);
	  decl_attributes (&d, chainon (postfix_attrs,
					all_prefix_attrs), 0);
	  DECL_CHAIN (d) = decls;
	  decls = d;
	  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	    all_prefix_attrs = chainon (c_parser_attributes (parser),
					prefix_attrs);
	  else
	    all_prefix_attrs = prefix_attrs;
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    c_parser_consume_token (parser);
	  else if (c_parser_next_token_is (parser, CPP_SEMICOLON)
		   || c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    {
	      /* Semicolon consumed in caller.  */
	      break;
	    }
	  else
	    {
	      c_parser_error (parser, "expected %<,%>, %<;%> or %<}%>");
	      break;
	    }
	}
      else
	{
	  c_parser_error (parser,
			  "expected %<:%>, %<,%>, %<;%>, %<}%> or "
			  "%<__attribute__%>");
	  break;
	}
    }
  return decls;
}

/* Parse a typeof specifier (a GNU extension).

   typeof-specifier:
     typeof ( expression )
     typeof ( type-name )
*/

static struct c_typespec
c_parser_typeof_specifier (c_parser *parser)
{
  struct c_typespec ret;
  ret.kind = ctsk_typeof;
  ret.spec = error_mark_node;
  ret.expr = NULL_TREE;
  ret.expr_const_operands = true;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_TYPEOF));
  c_parser_consume_token (parser);
  c_inhibit_evaluation_warnings++;
  in_typeof++;
  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      c_inhibit_evaluation_warnings--;
      in_typeof--;
      return ret;
    }
  if (c_parser_next_tokens_start_typename (parser, cla_prefer_id))
    {
      struct c_type_name *type = c_parser_type_name (parser);
      c_inhibit_evaluation_warnings--;
      in_typeof--;
      if (type != NULL)
	{
	  ret.spec = groktypename (type, &ret.expr, &ret.expr_const_operands);
	  pop_maybe_used (variably_modified_type_p (ret.spec, NULL_TREE));
	}
    }
  else
    {
      bool was_vm;
      location_t here = c_parser_peek_token (parser)->location;
      struct c_expr expr = c_parser_expression (parser);
      c_inhibit_evaluation_warnings--;
      in_typeof--;
      if (TREE_CODE (expr.value) == COMPONENT_REF
	  && DECL_C_BIT_FIELD (TREE_OPERAND (expr.value, 1)))
	error_at (here, "%<typeof%> applied to a bit-field");
      mark_exp_read (expr.value);
      ret.spec = TREE_TYPE (expr.value);
      was_vm = variably_modified_type_p (ret.spec, NULL_TREE);
      /* This is returned with the type so that when the type is
	 evaluated, this can be evaluated.  */
      if (was_vm)
	ret.expr = c_fully_fold (expr.value, false, &ret.expr_const_operands);
      pop_maybe_used (was_vm);
      /* For use in macros such as those in <stdatomic.h>, remove all
	 qualifiers from atomic types.  (const can be an issue for more macros
	 using typeof than just the <stdatomic.h> ones.)  */
      if (ret.spec != error_mark_node && TYPE_ATOMIC (ret.spec))
	ret.spec = c_build_qualified_type (ret.spec, TYPE_UNQUALIFIED);
    }
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return ret;
}

/* Parse an alignment-specifier.

   C11 6.7.5:

   alignment-specifier:
     _Alignas ( type-name )
     _Alignas ( constant-expression )
*/

static tree
c_parser_alignas_specifier (c_parser * parser)
{
  tree ret = error_mark_node;
  location_t loc = c_parser_peek_token (parser)->location;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ALIGNAS));
  c_parser_consume_token (parser);
  if (flag_isoc99)
    pedwarn_c99 (loc, OPT_Wpedantic,
		 "ISO C99 does not support %<_Alignas%>");
  else
    pedwarn_c99 (loc, OPT_Wpedantic,
		 "ISO C90 does not support %<_Alignas%>");
  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return ret;
  if (c_parser_next_tokens_start_typename (parser, cla_prefer_id))
    {
      struct c_type_name *type = c_parser_type_name (parser);
      if (type != NULL)
	ret = c_sizeof_or_alignof_type (loc, groktypename (type, NULL, NULL),
					false, true, 1);
    }
  else
    ret = c_parser_expr_no_commas (parser, NULL).value;
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return ret;
}

/* Parse a declarator, possibly an abstract declarator (C90 6.5.4,
   6.5.5, C99 6.7.5, 6.7.6).  If TYPE_SEEN_P then a typedef name may
   be redeclared; otherwise it may not.  KIND indicates which kind of
   declarator is wanted.  Returns a valid declarator except in the
   case of a syntax error in which case NULL is returned.  *SEEN_ID is
   set to true if an identifier being declared is seen; this is used
   to diagnose bad forms of abstract array declarators and to
   determine whether an identifier list is syntactically permitted.

   declarator:
     pointer[opt] direct-declarator

   direct-declarator:
     identifier
     ( attributes[opt] declarator )
     direct-declarator array-declarator
     direct-declarator ( parameter-type-list )
     direct-declarator ( identifier-list[opt] )

   pointer:
     * type-qualifier-list[opt]
     * type-qualifier-list[opt] pointer

   type-qualifier-list:
     type-qualifier
     attributes
     type-qualifier-list type-qualifier
     type-qualifier-list attributes

   array-declarator:
     [ type-qualifier-list[opt] assignment-expression[opt] ]
     [ static type-qualifier-list[opt] assignment-expression ]
     [ type-qualifier-list static assignment-expression ]
     [ type-qualifier-list[opt] * ]

   parameter-type-list:
     parameter-list
     parameter-list , ...

   parameter-list:
     parameter-declaration
     parameter-list , parameter-declaration

   parameter-declaration:
     declaration-specifiers declarator attributes[opt]
     declaration-specifiers abstract-declarator[opt] attributes[opt]

   identifier-list:
     identifier
     identifier-list , identifier

   abstract-declarator:
     pointer
     pointer[opt] direct-abstract-declarator

   direct-abstract-declarator:
     ( attributes[opt] abstract-declarator )
     direct-abstract-declarator[opt] array-declarator
     direct-abstract-declarator[opt] ( parameter-type-list[opt] )

   GNU extensions:

   direct-declarator:
     direct-declarator ( parameter-forward-declarations
			 parameter-type-list[opt] )

   direct-abstract-declarator:
     direct-abstract-declarator[opt] ( parameter-forward-declarations
				       parameter-type-list[opt] )

   parameter-forward-declarations:
     parameter-list ;
     parameter-forward-declarations parameter-list ;

   The uses of attributes shown above are GNU extensions.

   Some forms of array declarator are not included in C99 in the
   syntax for abstract declarators; these are disallowed elsewhere.
   This may be a defect (DR#289).

   This function also accepts an omitted abstract declarator as being
   an abstract declarator, although not part of the formal syntax.  */

static struct c_declarator *
c_parser_declarator (c_parser *parser, bool type_seen_p, c_dtr_syn kind,
		     bool *seen_id)
{
  /* Parse any initial pointer part.  */
  if (c_parser_next_token_is (parser, CPP_MULT))
    {
      struct c_declspecs *quals_attrs = build_null_declspecs ();
      struct c_declarator *inner;
      c_parser_consume_token (parser);
      c_parser_declspecs (parser, quals_attrs, false, false, true,
			  false, false, cla_prefer_id);
      inner = c_parser_declarator (parser, type_seen_p, kind, seen_id);
      if (inner == NULL)
	return NULL;
      else
	return make_pointer_declarator (quals_attrs, inner);
    }
  /* Now we have a direct declarator, direct abstract declarator or
     nothing (which counts as a direct abstract declarator here).  */
  return c_parser_direct_declarator (parser, type_seen_p, kind, seen_id);
}

/* Parse a direct declarator or direct abstract declarator; arguments
   as c_parser_declarator.  */

static struct c_declarator *
c_parser_direct_declarator (c_parser *parser, bool type_seen_p, c_dtr_syn kind,
			    bool *seen_id)
{
  /* The direct declarator must start with an identifier (possibly
     omitted) or a parenthesized declarator (possibly abstract).  In
     an ordinary declarator, initial parentheses must start a
     parenthesized declarator.  In an abstract declarator or parameter
     declarator, they could start a parenthesized declarator or a
     parameter list.  To tell which, the open parenthesis and any
     following attributes must be read.  If a declaration specifier
     follows, then it is a parameter list; if the specifier is a
     typedef name, there might be an ambiguity about redeclaring it,
     which is resolved in the direction of treating it as a typedef
     name.  If a close parenthesis follows, it is also an empty
     parameter list, as the syntax does not permit empty abstract
     declarators.  Otherwise, it is a parenthesized declarator (in
     which case the analysis may be repeated inside it, recursively).

     ??? There is an ambiguity in a parameter declaration "int
     (__attribute__((foo)) x)", where x is not a typedef name: it
     could be an abstract declarator for a function, or declare x with
     parentheses.  The proper resolution of this ambiguity needs
     documenting.  At present we follow an accident of the old
     parser's implementation, whereby the first parameter must have
     some declaration specifiers other than just attributes.  Thus as
     a parameter declaration it is treated as a parenthesized
     parameter named x, and as an abstract declarator it is
     rejected.

     ??? Also following the old parser, attributes inside an empty
     parameter list are ignored, making it a list not yielding a
     prototype, rather than giving an error or making it have one
     parameter with implicit type int.

     ??? Also following the old parser, typedef names may be
     redeclared in declarators, but not Objective-C class names.  */

  if (kind != C_DTR_ABSTRACT
      && c_parser_next_token_is (parser, CPP_NAME)
      && ((type_seen_p
	   && (c_parser_peek_token (parser)->id_kind == C_ID_TYPENAME
	       || c_parser_peek_token (parser)->id_kind == C_ID_CLASSNAME))
	  || c_parser_peek_token (parser)->id_kind == C_ID_ID))
    {
      struct c_declarator *inner
	= build_id_declarator (c_parser_peek_token (parser)->value);
      *seen_id = true;
      inner->id_loc = c_parser_peek_token (parser)->location;
      c_parser_consume_token (parser);
      return c_parser_direct_declarator_inner (parser, *seen_id, inner);
    }

  if (kind != C_DTR_NORMAL
      && c_parser_next_token_is (parser, CPP_OPEN_SQUARE))
    {
      struct c_declarator *inner = build_id_declarator (NULL_TREE);
      return c_parser_direct_declarator_inner (parser, *seen_id, inner);
    }

  /* Either we are at the end of an abstract declarator, or we have
     parentheses.  */

  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      tree attrs;
      struct c_declarator *inner;
      c_parser_consume_token (parser);
      attrs = c_parser_attributes (parser);
      if (kind != C_DTR_NORMAL
	  && (c_parser_next_token_starts_declspecs (parser)
	      || c_parser_next_token_is (parser, CPP_CLOSE_PAREN)))
	{
	  struct c_arg_info *args
	    = c_parser_parms_declarator (parser, kind == C_DTR_NORMAL,
					 attrs);
	  if (args == NULL)
	    return NULL;
	  else
	    {
	      inner
		= build_function_declarator (args,
					     build_id_declarator (NULL_TREE));
	      return c_parser_direct_declarator_inner (parser, *seen_id,
						       inner);
	    }
	}
      /* A parenthesized declarator.  */
      inner = c_parser_declarator (parser, type_seen_p, kind, seen_id);
      if (inner != NULL && attrs != NULL)
	inner = build_attrs_declarator (attrs, inner);
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  c_parser_consume_token (parser);
	  if (inner == NULL)
	    return NULL;
	  else
	    return c_parser_direct_declarator_inner (parser, *seen_id, inner);
	}
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return NULL;
	}
    }
  else
    {
      if (kind == C_DTR_NORMAL)
	{
	  c_parser_error (parser, "expected identifier or %<(%>");
	  return NULL;
	}
      else
	return build_id_declarator (NULL_TREE);
    }
}

/* Parse part of a direct declarator or direct abstract declarator,
   given that some (in INNER) has already been parsed; ID_PRESENT is
   true if an identifier is present, false for an abstract
   declarator.  */

static struct c_declarator *
c_parser_direct_declarator_inner (c_parser *parser, bool id_present,
				  struct c_declarator *inner)
{
  /* Parse a sequence of array declarators and parameter lists.  */
  if (c_parser_next_token_is (parser, CPP_OPEN_SQUARE))
    {
      location_t brace_loc = c_parser_peek_token (parser)->location;
      struct c_declarator *declarator;
      struct c_declspecs *quals_attrs = build_null_declspecs ();
      bool static_seen;
      bool star_seen;
      struct c_expr dimen;
      dimen.value = NULL_TREE;
      dimen.original_code = ERROR_MARK;
      dimen.original_type = NULL_TREE;
      c_parser_consume_token (parser);
      c_parser_declspecs (parser, quals_attrs, false, false, true,
			  false, false, cla_prefer_id);
      static_seen = c_parser_next_token_is_keyword (parser, RID_STATIC);
      if (static_seen)
	c_parser_consume_token (parser);
      if (static_seen && !quals_attrs->declspecs_seen_p)
	c_parser_declspecs (parser, quals_attrs, false, false, true,
			    false, false, cla_prefer_id);
      if (!quals_attrs->declspecs_seen_p)
	quals_attrs = NULL;
      /* If "static" is present, there must be an array dimension.
	 Otherwise, there may be a dimension, "*", or no
	 dimension.  */
      if (static_seen)
	{
	  star_seen = false;
	  dimen = c_parser_expr_no_commas (parser, NULL);
	}
      else
	{
	  if (c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
	    {
	      dimen.value = NULL_TREE;
	      star_seen = false;
	    }
	  else if (flag_cilkplus
		   && c_parser_next_token_is (parser, CPP_COLON))
	    {
	      dimen.value = error_mark_node;
	      star_seen = false;
	      error_at (c_parser_peek_token (parser)->location,
			"array notations cannot be used in declaration");
	      c_parser_consume_token (parser);
	    }   
	  else if (c_parser_next_token_is (parser, CPP_MULT))
	    {
	      if (c_parser_peek_2nd_token (parser)->type == CPP_CLOSE_SQUARE)
		{
		  dimen.value = NULL_TREE;
		  star_seen = true;
		  c_parser_consume_token (parser);
		}
	      else
		{
		  star_seen = false;
		  dimen = c_parser_expr_no_commas (parser, NULL);
		}
	    }
	  else
	    {
	      star_seen = false;
	      dimen = c_parser_expr_no_commas (parser, NULL);
	    }
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
	c_parser_consume_token (parser);
      else if (flag_cilkplus
	       && c_parser_next_token_is (parser, CPP_COLON))
	{
	  error_at (c_parser_peek_token (parser)->location,
		    "array notations cannot be used in declaration");
	  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	  return NULL;
	}
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
				     "expected %<]%>");
	  return NULL;
	}
      if (dimen.value)
	dimen = convert_lvalue_to_rvalue (brace_loc, dimen, true, true);
      declarator = build_array_declarator (brace_loc, dimen.value, quals_attrs,
					   static_seen, star_seen);
      if (declarator == NULL)
	return NULL;
      inner = set_array_declarator_inner (declarator, inner);
      return c_parser_direct_declarator_inner (parser, id_present, inner);
    }
  else if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      tree attrs;
      struct c_arg_info *args;
      c_parser_consume_token (parser);
      attrs = c_parser_attributes (parser);
      args = c_parser_parms_declarator (parser, id_present, attrs);
      if (args == NULL)
	return NULL;
      else
	{
	  inner = build_function_declarator (args, inner);
	  return c_parser_direct_declarator_inner (parser, id_present, inner);
	}
    }
  return inner;
}

/* Parse a parameter list or identifier list, including the closing
   parenthesis but not the opening one.  ATTRS are the attributes at
   the start of the list.  ID_LIST_OK is true if an identifier list is
   acceptable; such a list must not have attributes at the start.  */

static struct c_arg_info *
c_parser_parms_declarator (c_parser *parser, bool id_list_ok, tree attrs)
{
  push_scope ();
  declare_parm_level ();
  /* If the list starts with an identifier, it is an identifier list.
     Otherwise, it is either a prototype list or an empty list.  */
  if (id_list_ok
      && !attrs
      && c_parser_next_token_is (parser, CPP_NAME)
      && c_parser_peek_token (parser)->id_kind == C_ID_ID
      
      /* Look ahead to detect typos in type names.  */
      && c_parser_peek_2nd_token (parser)->type != CPP_NAME
      && c_parser_peek_2nd_token (parser)->type != CPP_MULT
      && c_parser_peek_2nd_token (parser)->type != CPP_OPEN_PAREN
      && c_parser_peek_2nd_token (parser)->type != CPP_OPEN_SQUARE)
    {
      tree list = NULL_TREE, *nextp = &list;
      while (c_parser_next_token_is (parser, CPP_NAME)
	     && c_parser_peek_token (parser)->id_kind == C_ID_ID)
	{
	  *nextp = build_tree_list (NULL_TREE,
				    c_parser_peek_token (parser)->value);
	  nextp = & TREE_CHAIN (*nextp);
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is_not (parser, CPP_COMMA))
	    break;
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    {
	      c_parser_error (parser, "expected identifier");
	      break;
	    }
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  struct c_arg_info *ret = build_arg_info ();
	  ret->types = list;
	  c_parser_consume_token (parser);
	  pop_scope ();
	  return ret;
	}
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  pop_scope ();
	  return NULL;
	}
    }
  else
    {
      struct c_arg_info *ret = c_parser_parms_list_declarator (parser, attrs,
							       NULL);
      pop_scope ();
      return ret;
    }
}

/* Parse a parameter list (possibly empty), including the closing
   parenthesis but not the opening one.  ATTRS are the attributes at
   the start of the list.  EXPR is NULL or an expression that needs to
   be evaluated for the side effects of array size expressions in the
   parameters.  */

static struct c_arg_info *
c_parser_parms_list_declarator (c_parser *parser, tree attrs, tree expr)
{
  bool bad_parm = false;

  /* ??? Following the old parser, forward parameter declarations may
     use abstract declarators, and if no real parameter declarations
     follow the forward declarations then this is not diagnosed.  Also
     note as above that attributes are ignored as the only contents of
     the parentheses, or as the only contents after forward
     declarations.  */
  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
    {
      struct c_arg_info *ret = build_arg_info ();
      c_parser_consume_token (parser);
      return ret;
    }
  if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
    {
      struct c_arg_info *ret = build_arg_info ();

      if (flag_allow_parameterless_variadic_functions)
        {
          /* F (...) is allowed.  */
          ret->types = NULL_TREE;
        }
      else
        {
          /* Suppress -Wold-style-definition for this case.  */
          ret->types = error_mark_node;
          error_at (c_parser_peek_token (parser)->location,
                    "ISO C requires a named argument before %<...%>");
        }
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  c_parser_consume_token (parser);
	  return ret;
	}
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return NULL;
	}
    }
  /* Nonempty list of parameters, either terminated with semicolon
     (forward declarations; recurse) or with close parenthesis (normal
     function) or with ", ... )" (variadic function).  */
  while (true)
    {
      /* Parse a parameter.  */
      struct c_parm *parm = c_parser_parameter_declaration (parser, attrs);
      attrs = NULL_TREE;
      if (parm == NULL)
	bad_parm = true;
      else
	push_parm_decl (parm, &expr);
      if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	{
	  tree new_attrs;
	  c_parser_consume_token (parser);
	  mark_forward_parm_decls ();
	  new_attrs = c_parser_attributes (parser);
	  return c_parser_parms_list_declarator (parser, new_attrs, expr);
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  c_parser_consume_token (parser);
	  if (bad_parm)
	    return NULL;
	  else
	    return get_parm_info (false, expr);
	}
      if (!c_parser_require (parser, CPP_COMMA,
			     "expected %<;%>, %<,%> or %<)%>"))
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return NULL;
	}
      if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
	{
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    {
	      c_parser_consume_token (parser);
	      if (bad_parm)
		return NULL;
	      else
		return get_parm_info (true, expr);
	    }
	  else
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
					 "expected %<)%>");
	      return NULL;
	    }
	}
    }
}

/* Parse a parameter declaration.  ATTRS are the attributes at the
   start of the declaration if it is the first parameter.  */

static struct c_parm *
c_parser_parameter_declaration (c_parser *parser, tree attrs)
{
  struct c_declspecs *specs;
  struct c_declarator *declarator;
  tree prefix_attrs;
  tree postfix_attrs = NULL_TREE;
  bool dummy = false;

  /* Accept #pragmas between parameter declarations.  */
  while (c_parser_next_token_is (parser, CPP_PRAGMA))
    c_parser_pragma (parser, pragma_param);

  if (!c_parser_next_token_starts_declspecs (parser))
    {
      c_token *token = c_parser_peek_token (parser);
      if (parser->error)
	return NULL;
      c_parser_set_source_position_from_token (token);
      if (c_parser_next_tokens_start_typename (parser, cla_prefer_type))
	{
	  error_at (token->location, "unknown type name %qE", token->value);
	  parser->error = true;
	}
      /* ??? In some Objective-C cases '...' isn't applicable so there
	 should be a different message.  */
      else
	c_parser_error (parser,
			"expected declaration specifiers or %<...%>");
      c_parser_skip_to_end_of_parameter (parser);
      return NULL;
    }
  specs = build_null_declspecs ();
  if (attrs)
    {
      declspecs_add_attrs (input_location, specs, attrs);
      attrs = NULL_TREE;
    }
  c_parser_declspecs (parser, specs, true, true, true, true, false,
		      cla_nonabstract_decl);
  finish_declspecs (specs);
  pending_xref_error ();
  prefix_attrs = specs->attrs;
  specs->attrs = NULL_TREE;
  declarator = c_parser_declarator (parser,
				    specs->typespec_kind != ctsk_none,
				    C_DTR_PARM, &dummy);
  if (declarator == NULL)
    {
      c_parser_skip_until_found (parser, CPP_COMMA, NULL);
      return NULL;
    }
  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
    postfix_attrs = c_parser_attributes (parser);
  return build_c_parm (specs, chainon (postfix_attrs, prefix_attrs),
		       declarator);
}

/* Parse a string literal in an asm expression.  It should not be
   translated, and wide string literals are an error although
   permitted by the syntax.  This is a GNU extension.

   asm-string-literal:
     string-literal

   ??? At present, following the old parser, the caller needs to have
   set lex_untranslated_string to 1.  It would be better to follow the
   C++ parser rather than using this kludge.  */

static tree
c_parser_asm_string_literal (c_parser *parser)
{
  tree str;
  int save_flag = warn_overlength_strings;
  warn_overlength_strings = 0;
  if (c_parser_next_token_is (parser, CPP_STRING))
    {
      str = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
    }
  else if (c_parser_next_token_is (parser, CPP_WSTRING))
    {
      error_at (c_parser_peek_token (parser)->location,
		"wide string literal in %<asm%>");
      str = build_string (1, "");
      c_parser_consume_token (parser);
    }
  else
    {
      c_parser_error (parser, "expected string literal");
      str = NULL_TREE;
    }
  warn_overlength_strings = save_flag;
  return str;
}

/* Parse a simple asm expression.  This is used in restricted
   contexts, where a full expression with inputs and outputs does not
   make sense.  This is a GNU extension.

   simple-asm-expr:
     asm ( asm-string-literal )
*/

static tree
c_parser_simple_asm_expr (c_parser *parser)
{
  tree str;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ASM));
  /* ??? Follow the C++ parser rather than using the
     lex_untranslated_string kludge.  */
  parser->lex_untranslated_string = true;
  c_parser_consume_token (parser);
  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      parser->lex_untranslated_string = false;
      return NULL_TREE;
    }
  str = c_parser_asm_string_literal (parser);
  parser->lex_untranslated_string = false;
  if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return NULL_TREE;
    }
  return str;
}

static tree
c_parser_attribute_any_word (c_parser *parser)
{
  tree attr_name = NULL_TREE;

  if (c_parser_next_token_is (parser, CPP_KEYWORD))
    {
      /* ??? See comment above about what keywords are accepted here.  */
      bool ok;
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_STATIC:
	case RID_UNSIGNED:
	case RID_LONG:
	case RID_CONST:
	case RID_EXTERN:
	case RID_REGISTER:
	case RID_TYPEDEF:
	case RID_SHORT:
	case RID_INLINE:
	case RID_NORETURN:
	case RID_VOLATILE:
	case RID_SIGNED:
	case RID_AUTO:
	case RID_RESTRICT:
	case RID_COMPLEX:
	case RID_THREAD:
	case RID_INT:
	case RID_CHAR:
	case RID_FLOAT:
	case RID_DOUBLE:
	case RID_VOID:
	case RID_DFLOAT32:
	case RID_DFLOAT64:
	case RID_DFLOAT128:
	case RID_BOOL:
	case RID_FRACT:
	case RID_ACCUM:
	case RID_SAT:
	case RID_TRANSACTION_ATOMIC:
	case RID_TRANSACTION_CANCEL:
	case RID_ATOMIC:
	case RID_AUTO_TYPE:
	case RID_INT_N_0:
	case RID_INT_N_1:
	case RID_INT_N_2:
	case RID_INT_N_3:
	  ok = true;
	  break;
	default:
	  ok = false;
	  break;
	}
      if (!ok)
	return NULL_TREE;

      /* Accept __attribute__((__const)) as __attribute__((const)) etc.  */
      attr_name = ridpointers[(int) c_parser_peek_token (parser)->keyword];
    }
  else if (c_parser_next_token_is (parser, CPP_NAME))
    attr_name = c_parser_peek_token (parser)->value;

  return attr_name;
}

/* Returns true of NAME is an IDENTIFIER_NODE with identiifer "vector,"
   "__vector" or "__vector__."  */

static inline bool
is_cilkplus_vector_p (tree name)
{ 
  if (flag_cilkplus && is_attribute_p ("vector", name)) 
    return true;
  return false;
}

#define CILK_SIMD_FN_CLAUSE_MASK				  \
	((OMP_CLAUSE_MASK_1 << PRAGMA_CILK_CLAUSE_VECTORLENGTH)	  \
	 | (OMP_CLAUSE_MASK_1 << PRAGMA_CILK_CLAUSE_LINEAR)	  \
	 | (OMP_CLAUSE_MASK_1 << PRAGMA_CILK_CLAUSE_UNIFORM)	  \
	 | (OMP_CLAUSE_MASK_1 << PRAGMA_CILK_CLAUSE_MASK)	  \
	 | (OMP_CLAUSE_MASK_1 << PRAGMA_CILK_CLAUSE_NOMASK))

/* Parses the vector attribute of SIMD enabled functions in Cilk Plus.
   VEC_TOKEN is the "vector" token that is replaced with "simd" and
   pushed into the token list. 
   Syntax:
   vector
   vector (<vector attributes>).  */

static void
c_parser_cilk_simd_fn_vector_attrs (c_parser *parser, c_token vec_token)
{
  gcc_assert (is_cilkplus_vector_p (vec_token.value));
  
  int paren_scope = 0;
  vec_safe_push (parser->cilk_simd_fn_tokens, vec_token);
  /* Consume the "vector" token.  */
  c_parser_consume_token (parser);

  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      c_parser_consume_token (parser);
      paren_scope++;
    }
  while (paren_scope > 0)
    {
      c_token *token = c_parser_peek_token (parser);
      if (token->type == CPP_OPEN_PAREN)
        paren_scope++;
      else if (token->type == CPP_CLOSE_PAREN)
        paren_scope--;
      /* Do not push the last ')' since we are not pushing the '('.  */
      if (!(token->type == CPP_CLOSE_PAREN && paren_scope == 0))
	vec_safe_push (parser->cilk_simd_fn_tokens, *token);
      c_parser_consume_token (parser);
    }
  
  /* Since we are converting an attribute to a pragma, we need to end the
     attribute with PRAGMA_EOL.  */
  c_token eol_token;
  memset (&eol_token, 0, sizeof (eol_token));
  eol_token.type = CPP_PRAGMA_EOL;
  vec_safe_push (parser->cilk_simd_fn_tokens, eol_token);
}

/* Add 2 CPP_EOF at the end of PARSER->ELEM_FN_TOKENS vector.  */

static void
c_finish_cilk_simd_fn_tokens (c_parser *parser)
{
  c_token last_token = parser->cilk_simd_fn_tokens->last ();

  /* c_parser_attributes is called in several places, so if these EOF
     tokens are already inserted, then don't do them again.  */
  if (last_token.type == CPP_EOF)
    return;
  
  /* Two CPP_EOF token are added as a safety net since the normal C
     front-end has two token look-ahead.  */
  c_token eof_token;
  eof_token.type = CPP_EOF;
  vec_safe_push (parser->cilk_simd_fn_tokens, eof_token);
  vec_safe_push (parser->cilk_simd_fn_tokens, eof_token);
}

/* Parse (possibly empty) attributes.  This is a GNU extension.

   attributes:
     empty
     attributes attribute

   attribute:
     __attribute__ ( ( attribute-list ) )

   attribute-list:
     attrib
     attribute_list , attrib

   attrib:
     empty
     any-word
     any-word ( identifier )
     any-word ( identifier , nonempty-expr-list )
     any-word ( expr-list )

   where the "identifier" must not be declared as a type, and
   "any-word" may be any identifier (including one declared as a
   type), a reserved word storage class specifier, type specifier or
   type qualifier.  ??? This still leaves out most reserved keywords
   (following the old parser), shouldn't we include them, and why not
   allow identifiers declared as types to start the arguments?  */

static tree
c_parser_attributes (c_parser *parser)
{
  tree attrs = NULL_TREE;
  while (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
    {
      /* ??? Follow the C++ parser rather than using the
	 lex_untranslated_string kludge.  */
      parser->lex_untranslated_string = true;
      c_parser_consume_token (parser);
      if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	{
	  parser->lex_untranslated_string = false;
	  return attrs;
	}
      if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	{
	  parser->lex_untranslated_string = false;
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return attrs;
	}
      /* Parse the attribute list.  */
      while (c_parser_next_token_is (parser, CPP_COMMA)
	     || c_parser_next_token_is (parser, CPP_NAME)
	     || c_parser_next_token_is (parser, CPP_KEYWORD))
	{
	  tree attr, attr_name, attr_args;
	  vec<tree, va_gc> *expr_list;
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    {
	      c_parser_consume_token (parser);
	      continue;
	    }

	  attr_name = c_parser_attribute_any_word (parser);
	  if (attr_name == NULL)
	    break;
	  if (is_cilkplus_vector_p (attr_name))		  
	    {
	      c_token *v_token = c_parser_peek_token (parser);
	      c_parser_cilk_simd_fn_vector_attrs (parser, *v_token);
	      continue;
	    }
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is_not (parser, CPP_OPEN_PAREN))
	    {
	      attr = build_tree_list (attr_name, NULL_TREE);
	      attrs = chainon (attrs, attr);
	      continue;
	    }
	  c_parser_consume_token (parser);
	  /* Parse the attribute contents.  If they start with an
	     identifier which is followed by a comma or close
	     parenthesis, then the arguments start with that
	     identifier; otherwise they are an expression list.  
	     In objective-c the identifier may be a classname.  */
	  if (c_parser_next_token_is (parser, CPP_NAME)
	      && (c_parser_peek_token (parser)->id_kind == C_ID_ID
		  || (c_dialect_objc ()
		      && c_parser_peek_token (parser)->id_kind
			 == C_ID_CLASSNAME))
	      && ((c_parser_peek_2nd_token (parser)->type == CPP_COMMA)
		  || (c_parser_peek_2nd_token (parser)->type
		      == CPP_CLOSE_PAREN))
	      && (attribute_takes_identifier_p (attr_name)
		  || (c_dialect_objc ()
		      && c_parser_peek_token (parser)->id_kind
			 == C_ID_CLASSNAME)))
	    {
	      tree arg1 = c_parser_peek_token (parser)->value;
	      c_parser_consume_token (parser);
	      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
		attr_args = build_tree_list (NULL_TREE, arg1);
	      else
		{
		  tree tree_list;
		  c_parser_consume_token (parser);
		  expr_list = c_parser_expr_list (parser, false, true,
						  NULL, NULL, NULL, NULL);
		  tree_list = build_tree_list_vec (expr_list);
		  attr_args = tree_cons (NULL_TREE, arg1, tree_list);
		  release_tree_vector (expr_list);
		}
	    }
	  else
	    {
	      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
		attr_args = NULL_TREE;
	      else
		{
		  expr_list = c_parser_expr_list (parser, false, true,
						  NULL, NULL, NULL, NULL);
		  attr_args = build_tree_list_vec (expr_list);
		  release_tree_vector (expr_list);
		}
	    }
	  attr = build_tree_list (attr_name, attr_args);
	  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    c_parser_consume_token (parser);
	  else
	    {
	      parser->lex_untranslated_string = false;
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
					 "expected %<)%>");
	      return attrs;
	    }
	  attrs = chainon (attrs, attr);
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	c_parser_consume_token (parser);
      else
	{
	  parser->lex_untranslated_string = false;
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return attrs;
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	c_parser_consume_token (parser);
      else
	{
	  parser->lex_untranslated_string = false;
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return attrs;
	}
      parser->lex_untranslated_string = false;
    }

  if (flag_cilkplus && !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
    c_finish_cilk_simd_fn_tokens (parser);
  return attrs;
}

/* Parse a type name (C90 6.5.5, C99 6.7.6).

   type-name:
     specifier-qualifier-list abstract-declarator[opt]
*/

static struct c_type_name *
c_parser_type_name (c_parser *parser)
{
  struct c_declspecs *specs = build_null_declspecs ();
  struct c_declarator *declarator;
  struct c_type_name *ret;
  bool dummy = false;
  c_parser_declspecs (parser, specs, false, true, true, false, false,
		      cla_prefer_type);
  if (!specs->declspecs_seen_p)
    {
      c_parser_error (parser, "expected specifier-qualifier-list");
      return NULL;
    }
  if (specs->type != error_mark_node)
    {
      pending_xref_error ();
      finish_declspecs (specs);
    }
  declarator = c_parser_declarator (parser,
				    specs->typespec_kind != ctsk_none,
				    C_DTR_ABSTRACT, &dummy);
  if (declarator == NULL)
    return NULL;
  ret = XOBNEW (&parser_obstack, struct c_type_name);
  ret->specs = specs;
  ret->declarator = declarator;
  return ret;
}

/* Parse an initializer (C90 6.5.7, C99 6.7.8).

   initializer:
     assignment-expression
     { initializer-list }
     { initializer-list , }

   initializer-list:
     designation[opt] initializer
     initializer-list , designation[opt] initializer

   designation:
     designator-list =

   designator-list:
     designator
     designator-list designator

   designator:
     array-designator
     . identifier

   array-designator:
     [ constant-expression ]

   GNU extensions:

   initializer:
     { }

   designation:
     array-designator
     identifier :

   array-designator:
     [ constant-expression ... constant-expression ]

   Any expression without commas is accepted in the syntax for the
   constant-expressions, with non-constant expressions rejected later.

   This function is only used for top-level initializers; for nested
   ones, see c_parser_initval.  */

static struct c_expr
c_parser_initializer (c_parser *parser)
{
  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    return c_parser_braced_init (parser, NULL_TREE, false);
  else
    {
      struct c_expr ret;
      location_t loc = c_parser_peek_token (parser)->location;
      ret = c_parser_expr_no_commas (parser, NULL);
      if (TREE_CODE (ret.value) != STRING_CST
	  && TREE_CODE (ret.value) != COMPOUND_LITERAL_EXPR)
	ret = convert_lvalue_to_rvalue (loc, ret, true, true);
      return ret;
    }
}

/* Parse a braced initializer list.  TYPE is the type specified for a
   compound literal, and NULL_TREE for other initializers and for
   nested braced lists.  NESTED_P is true for nested braced lists,
   false for the list of a compound literal or the list that is the
   top-level initializer in a declaration.  */

static struct c_expr
c_parser_braced_init (c_parser *parser, tree type, bool nested_p)
{
  struct c_expr ret;
  struct obstack braced_init_obstack;
  location_t brace_loc = c_parser_peek_token (parser)->location;
  gcc_obstack_init (&braced_init_obstack);
  gcc_assert (c_parser_next_token_is (parser, CPP_OPEN_BRACE));
  c_parser_consume_token (parser);
  if (nested_p)
    push_init_level (brace_loc, 0, &braced_init_obstack);
  else
    really_start_incremental_init (type);
  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      pedwarn (brace_loc, OPT_Wpedantic, "ISO C forbids empty initializer braces");
    }
  else
    {
      /* Parse a non-empty initializer list, possibly with a trailing
	 comma.  */
      while (true)
	{
	  c_parser_initelt (parser, &braced_init_obstack);
	  if (parser->error)
	    break;
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    c_parser_consume_token (parser);
	  else
	    break;
	  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    break;
	}
    }
  if (c_parser_next_token_is_not (parser, CPP_CLOSE_BRACE))
    {
      ret.value = error_mark_node;
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, "expected %<}%>");
      pop_init_level (brace_loc, 0, &braced_init_obstack);
      obstack_free (&braced_init_obstack, NULL);
      return ret;
    }
  c_parser_consume_token (parser);
  ret = pop_init_level (brace_loc, 0, &braced_init_obstack);
  obstack_free (&braced_init_obstack, NULL);
  return ret;
}

/* Parse a nested initializer, including designators.  */

static void
c_parser_initelt (c_parser *parser, struct obstack * braced_init_obstack)
{
  /* Parse any designator or designator list.  A single array
     designator may have the subsequent "=" omitted in GNU C, but a
     longer list or a structure member designator may not.  */
  if (c_parser_next_token_is (parser, CPP_NAME)
      && c_parser_peek_2nd_token (parser)->type == CPP_COLON)
    {
      /* Old-style structure member designator.  */
      set_init_label (c_parser_peek_token (parser)->location,
		      c_parser_peek_token (parser)->value,
		      braced_init_obstack);
      /* Use the colon as the error location.  */
      pedwarn (c_parser_peek_2nd_token (parser)->location, OPT_Wpedantic,
	       "obsolete use of designated initializer with %<:%>");
      c_parser_consume_token (parser);
      c_parser_consume_token (parser);
    }
  else
    {
      /* des_seen is 0 if there have been no designators, 1 if there
	 has been a single array designator and 2 otherwise.  */
      int des_seen = 0;
      /* Location of a designator.  */
      location_t des_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
      while (c_parser_next_token_is (parser, CPP_OPEN_SQUARE)
	     || c_parser_next_token_is (parser, CPP_DOT))
	{
	  int des_prev = des_seen;
	  if (!des_seen)
	    des_loc = c_parser_peek_token (parser)->location;
	  if (des_seen < 2)
	    des_seen++;
	  if (c_parser_next_token_is (parser, CPP_DOT))
	    {
	      des_seen = 2;
	      c_parser_consume_token (parser);
	      if (c_parser_next_token_is (parser, CPP_NAME))
		{
		  set_init_label (des_loc, c_parser_peek_token (parser)->value,
				  braced_init_obstack);
		  c_parser_consume_token (parser);
		}
	      else
		{
		  struct c_expr init;
		  init.value = error_mark_node;
		  init.original_code = ERROR_MARK;
		  init.original_type = NULL;
		  c_parser_error (parser, "expected identifier");
		  c_parser_skip_until_found (parser, CPP_COMMA, NULL);
		  process_init_element (input_location, init, false,
					braced_init_obstack);
		  return;
		}
	    }
	  else
	    {
	      tree first, second;
	      location_t ellipsis_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
	      location_t array_index_loc = UNKNOWN_LOCATION;
	      /* ??? Following the old parser, [ objc-receiver
		 objc-message-args ] is accepted as an initializer,
		 being distinguished from a designator by what follows
		 the first assignment expression inside the square
		 brackets, but after a first array designator a
		 subsequent square bracket is for Objective-C taken to
		 start an expression, using the obsolete form of
		 designated initializer without '=', rather than
		 possibly being a second level of designation: in LALR
		 terms, the '[' is shifted rather than reducing
		 designator to designator-list.  */
	      if (des_prev == 1 && c_dialect_objc ())
		{
		  des_seen = des_prev;
		  break;
		}
	      if (des_prev == 0 && c_dialect_objc ())
		{
		  /* This might be an array designator or an
		     Objective-C message expression.  If the former,
		     continue parsing here; if the latter, parse the
		     remainder of the initializer given the starting
		     primary-expression.  ??? It might make sense to
		     distinguish when des_prev == 1 as well; see
		     previous comment.  */
		  tree rec, args;
		  struct c_expr mexpr;
		  c_parser_consume_token (parser);
		  if (c_parser_peek_token (parser)->type == CPP_NAME
		      && ((c_parser_peek_token (parser)->id_kind
			   == C_ID_TYPENAME)
			  || (c_parser_peek_token (parser)->id_kind
			      == C_ID_CLASSNAME)))
		    {
		      /* Type name receiver.  */
		      tree id = c_parser_peek_token (parser)->value;
		      c_parser_consume_token (parser);
		      rec = objc_get_class_reference (id);
		      goto parse_message_args;
		    }
		  first = c_parser_expr_no_commas (parser, NULL).value;
		  mark_exp_read (first);
		  if (c_parser_next_token_is (parser, CPP_ELLIPSIS)
		      || c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
		    goto array_desig_after_first;
		  /* Expression receiver.  So far only one part
		     without commas has been parsed; there might be
		     more of the expression.  */
		  rec = first;
		  while (c_parser_next_token_is (parser, CPP_COMMA))
		    {
		      struct c_expr next;
		      location_t comma_loc, exp_loc;
		      comma_loc = c_parser_peek_token (parser)->location;
		      c_parser_consume_token (parser);
		      exp_loc = c_parser_peek_token (parser)->location;
		      next = c_parser_expr_no_commas (parser, NULL);
		      next = convert_lvalue_to_rvalue (exp_loc, next,
						       true, true);
		      rec = build_compound_expr (comma_loc, rec, next.value);
		    }
		parse_message_args:
		  /* Now parse the objc-message-args.  */
		  args = c_parser_objc_message_args (parser);
		  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
					     "expected %<]%>");
		  mexpr.value
		    = objc_build_message_expr (rec, args);
		  mexpr.original_code = ERROR_MARK;
		  mexpr.original_type = NULL;
		  /* Now parse and process the remainder of the
		     initializer, starting with this message
		     expression as a primary-expression.  */
		  c_parser_initval (parser, &mexpr, braced_init_obstack);
		  return;
		}
	      c_parser_consume_token (parser);
	      array_index_loc = c_parser_peek_token (parser)->location;
	      first = c_parser_expr_no_commas (parser, NULL).value;
	      mark_exp_read (first);
	    array_desig_after_first:
	      if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
		{
		  ellipsis_loc = c_parser_peek_token (parser)->location;
		  c_parser_consume_token (parser);
		  second = c_parser_expr_no_commas (parser, NULL).value;
		  mark_exp_read (second);
		}
	      else
		second = NULL_TREE;
	      if (c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
		{
		  c_parser_consume_token (parser);
		  set_init_index (array_index_loc, first, second,
				  braced_init_obstack);
		  if (second)
		    pedwarn (ellipsis_loc, OPT_Wpedantic,
			     "ISO C forbids specifying range of elements to initialize");
		}
	      else
		c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
					   "expected %<]%>");
	    }
	}
      if (des_seen >= 1)
	{
	  if (c_parser_next_token_is (parser, CPP_EQ))
	    {
	      pedwarn_c90 (des_loc, OPT_Wpedantic,
			   "ISO C90 forbids specifying subobject "
			   "to initialize");
	      c_parser_consume_token (parser);
	    }
	  else
	    {
	      if (des_seen == 1)
		pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
			 "obsolete use of designated initializer without %<=%>");
	      else
		{
		  struct c_expr init;
		  init.value = error_mark_node;
		  init.original_code = ERROR_MARK;
		  init.original_type = NULL;
		  c_parser_error (parser, "expected %<=%>");
		  c_parser_skip_until_found (parser, CPP_COMMA, NULL);
		  process_init_element (input_location, init, false,
					braced_init_obstack);
		  return;
		}
	    }
	}
    }
  c_parser_initval (parser, NULL, braced_init_obstack);
}

/* Parse a nested initializer; as c_parser_initializer but parses
   initializers within braced lists, after any designators have been
   applied.  If AFTER is not NULL then it is an Objective-C message
   expression which is the primary-expression starting the
   initializer.  */

static void
c_parser_initval (c_parser *parser, struct c_expr *after,
		  struct obstack * braced_init_obstack)
{
  struct c_expr init;
  gcc_assert (!after || c_dialect_objc ());
  location_t loc = c_parser_peek_token (parser)->location;

  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE) && !after)
    init = c_parser_braced_init (parser, NULL_TREE, true);
  else
    {
      init = c_parser_expr_no_commas (parser, after);
      if (init.value != NULL_TREE
	  && TREE_CODE (init.value) != STRING_CST
	  && TREE_CODE (init.value) != COMPOUND_LITERAL_EXPR)
	init = convert_lvalue_to_rvalue (loc, init, true, true);
    }
  process_init_element (loc, init, false, braced_init_obstack);
}

/* Parse a compound statement (possibly a function body) (C90 6.6.2,
   C99 6.8.2).

   compound-statement:
     { block-item-list[opt] }
     { label-declarations block-item-list }

   block-item-list:
     block-item
     block-item-list block-item

   block-item:
     nested-declaration
     statement

   nested-declaration:
     declaration

   GNU extensions:

   compound-statement:
     { label-declarations block-item-list }

   nested-declaration:
     __extension__ nested-declaration
     nested-function-definition

   label-declarations:
     label-declaration
     label-declarations label-declaration

   label-declaration:
     __label__ identifier-list ;

   Allowing the mixing of declarations and code is new in C99.  The
   GNU syntax also permits (not shown above) labels at the end of
   compound statements, which yield an error.  We don't allow labels
   on declarations; this might seem like a natural extension, but
   there would be a conflict between attributes on the label and
   prefix attributes on the declaration.  ??? The syntax follows the
   old parser in requiring something after label declarations.
   Although they are erroneous if the labels declared aren't defined,
   is it useful for the syntax to be this way?

   OpenMP:

   block-item:
     openmp-directive

   openmp-directive:
     barrier-directive
     flush-directive
     taskwait-directive
     taskyield-directive
     cancel-directive
     cancellation-point-directive  */

static tree
c_parser_compound_statement (c_parser *parser)
{
  tree stmt;
  location_t brace_loc;
  brace_loc = c_parser_peek_token (parser)->location;
  if (!c_parser_require (parser, CPP_OPEN_BRACE, "expected %<{%>"))
    {
      /* Ensure a scope is entered and left anyway to avoid confusion
	 if we have just prepared to enter a function body.  */
      stmt = c_begin_compound_stmt (true);
      c_end_compound_stmt (brace_loc, stmt, true);
      return error_mark_node;
    }
  stmt = c_begin_compound_stmt (true);
  c_parser_compound_statement_nostart (parser);

  /* If the compound stmt contains array notations, then we expand them.  */
  if (flag_cilkplus && contains_array_notation_expr (stmt))
    stmt = expand_array_notation_exprs (stmt);
  return c_end_compound_stmt (brace_loc, stmt, true);
}

/* Parse a compound statement except for the opening brace.  This is
   used for parsing both compound statements and statement expressions
   (which follow different paths to handling the opening).  */

static void
c_parser_compound_statement_nostart (c_parser *parser)
{
  bool last_stmt = false;
  bool last_label = false;
  bool save_valid_for_pragma = valid_location_for_stdc_pragma_p ();
  location_t label_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      c_parser_consume_token (parser);
      return;
    }
  mark_valid_location_for_stdc_pragma (true);
  if (c_parser_next_token_is_keyword (parser, RID_LABEL))
    {
      /* Read zero or more forward-declarations for labels that nested
	 functions can jump to.  */
      mark_valid_location_for_stdc_pragma (false);
      while (c_parser_next_token_is_keyword (parser, RID_LABEL))
	{
	  label_loc = c_parser_peek_token (parser)->location;
	  c_parser_consume_token (parser);
	  /* Any identifiers, including those declared as type names,
	     are OK here.  */
	  while (true)
	    {
	      tree label;
	      if (c_parser_next_token_is_not (parser, CPP_NAME))
		{
		  c_parser_error (parser, "expected identifier");
		  break;
		}
	      label
		= declare_label (c_parser_peek_token (parser)->value);
	      C_DECLARED_LABEL_FLAG (label) = 1;
	      add_stmt (build_stmt (label_loc, DECL_EXPR, label));
	      c_parser_consume_token (parser);
	      if (c_parser_next_token_is (parser, CPP_COMMA))
		c_parser_consume_token (parser);
	      else
		break;
	    }
	  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
	}
      pedwarn (label_loc, OPT_Wpedantic, "ISO C forbids label declarations");
    }
  /* We must now have at least one statement, label or declaration.  */
  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
      c_parser_error (parser, "expected declaration or statement");
      c_parser_consume_token (parser);
      return;
    }
  while (c_parser_next_token_is_not (parser, CPP_CLOSE_BRACE))
    {
      location_t loc = c_parser_peek_token (parser)->location;
      if (c_parser_next_token_is_keyword (parser, RID_CASE)
	  || c_parser_next_token_is_keyword (parser, RID_DEFAULT)
	  || (c_parser_next_token_is (parser, CPP_NAME)
	      && c_parser_peek_2nd_token (parser)->type == CPP_COLON))
	{
	  if (c_parser_next_token_is_keyword (parser, RID_CASE))
	    label_loc = c_parser_peek_2nd_token (parser)->location;
	  else
	    label_loc = c_parser_peek_token (parser)->location;
	  last_label = true;
	  last_stmt = false;
	  mark_valid_location_for_stdc_pragma (false);
	  c_parser_label (parser);
	}
      else if (!last_label
	       && c_parser_next_tokens_start_declaration (parser))
	{
	  last_label = false;
	  mark_valid_location_for_stdc_pragma (false);
	  c_parser_declaration_or_fndef (parser, true, true, true, true,
					 true, NULL, vNULL);
	  if (last_stmt)
	    pedwarn_c90 (loc, OPT_Wdeclaration_after_statement,
			 "ISO C90 forbids mixed declarations and code");
	  last_stmt = false;
	}
      else if (!last_label
	       && c_parser_next_token_is_keyword (parser, RID_EXTENSION))
	{
	  /* __extension__ can start a declaration, but is also an
	     unary operator that can start an expression.  Consume all
	     but the last of a possible series of __extension__ to
	     determine which.  */
	  while (c_parser_peek_2nd_token (parser)->type == CPP_KEYWORD
		 && (c_parser_peek_2nd_token (parser)->keyword
		     == RID_EXTENSION))
	    c_parser_consume_token (parser);
	  if (c_token_starts_declaration (c_parser_peek_2nd_token (parser)))
	    {
	      int ext;
	      ext = disable_extension_diagnostics ();
	      c_parser_consume_token (parser);
	      last_label = false;
	      mark_valid_location_for_stdc_pragma (false);
	      c_parser_declaration_or_fndef (parser, true, true, true, true,
					     true, NULL, vNULL);
	      /* Following the old parser, __extension__ does not
		 disable this diagnostic.  */
	      restore_extension_diagnostics (ext);
	      if (last_stmt)
		pedwarn_c90 (loc, OPT_Wdeclaration_after_statement,
			     "ISO C90 forbids mixed declarations and code");
	      last_stmt = false;
	    }
	  else
	    goto statement;
	}
      else if (c_parser_next_token_is (parser, CPP_PRAGMA))
	{
	  /* External pragmas, and some omp pragmas, are not associated
	     with regular c code, and so are not to be considered statements
	     syntactically.  This ensures that the user doesn't put them
	     places that would turn into syntax errors if the directive
	     were ignored.  */
	  if (c_parser_pragma (parser, pragma_compound))
	    last_label = false, last_stmt = true;
	}
      else if (c_parser_next_token_is (parser, CPP_EOF))
	{
	  mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
	  c_parser_error (parser, "expected declaration or statement");
	  return;
	}
      else if (c_parser_next_token_is_keyword (parser, RID_ELSE))
        {
          if (parser->in_if_block)
            {
	      mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
              error_at (loc, """expected %<}%> before %<else%>");
              return;
            }
          else
            {
              error_at (loc, "%<else%> without a previous %<if%>");
              c_parser_consume_token (parser);
              continue;
            }
        }
      else
	{
	statement:
	  last_label = false;
	  last_stmt = true;
	  mark_valid_location_for_stdc_pragma (false);
	  c_parser_statement_after_labels (parser);
	}

      parser->error = false;
    }
  if (last_label)
    error_at (label_loc, "label at end of compound statement");
  c_parser_consume_token (parser);
  /* Restore the value we started with.  */
  mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
}

/* Parse all consecutive labels. */

static void
c_parser_all_labels (c_parser *parser)
{
  while (c_parser_next_token_is_keyword (parser, RID_CASE)
	 || c_parser_next_token_is_keyword (parser, RID_DEFAULT)
	 || (c_parser_next_token_is (parser, CPP_NAME)
	     && c_parser_peek_2nd_token (parser)->type == CPP_COLON))
    c_parser_label (parser);
}

/* Parse a label (C90 6.6.1, C99 6.8.1).

   label:
     identifier : attributes[opt]
     case constant-expression :
     default :

   GNU extensions:

   label:
     case constant-expression ... constant-expression :

   The use of attributes on labels is a GNU extension.  The syntax in
   GNU C accepts any expressions without commas, non-constant
   expressions being rejected later.  */

static void
c_parser_label (c_parser *parser)
{
  location_t loc1 = c_parser_peek_token (parser)->location;
  tree label = NULL_TREE;
  if (c_parser_next_token_is_keyword (parser, RID_CASE))
    {
      tree exp1, exp2;
      c_parser_consume_token (parser);
      exp1 = c_parser_expr_no_commas (parser, NULL).value;
      if (c_parser_next_token_is (parser, CPP_COLON))
	{
	  c_parser_consume_token (parser);
	  label = do_case (loc1, exp1, NULL_TREE);
	}
      else if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
	{
	  c_parser_consume_token (parser);
	  exp2 = c_parser_expr_no_commas (parser, NULL).value;
	  if (c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	    label = do_case (loc1, exp1, exp2);
	}
      else
	c_parser_error (parser, "expected %<:%> or %<...%>");
    }
  else if (c_parser_next_token_is_keyword (parser, RID_DEFAULT))
    {
      c_parser_consume_token (parser);
      if (c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	label = do_case (loc1, NULL_TREE, NULL_TREE);
    }
  else
    {
      tree name = c_parser_peek_token (parser)->value;
      tree tlab;
      tree attrs;
      location_t loc2 = c_parser_peek_token (parser)->location;
      gcc_assert (c_parser_next_token_is (parser, CPP_NAME));
      c_parser_consume_token (parser);
      gcc_assert (c_parser_next_token_is (parser, CPP_COLON));
      c_parser_consume_token (parser);
      attrs = c_parser_attributes (parser);
      tlab = define_label (loc2, name);
      if (tlab)
	{
	  decl_attributes (&tlab, attrs, 0);
	  label = add_stmt (build_stmt (loc1, LABEL_EXPR, tlab));
	}
    }
  if (label)
    {
      if (c_parser_next_tokens_start_declaration (parser))
	{
	  error_at (c_parser_peek_token (parser)->location,
		    "a label can only be part of a statement and "
		    "a declaration is not a statement");
	  c_parser_declaration_or_fndef (parser, /*fndef_ok*/ false,
					 /*static_assert_ok*/ true,
					 /*empty_ok*/ true, /*nested*/ true,
					 /*start_attr_ok*/ true, NULL,
					 vNULL);
	}
    }
}

/* Parse a statement (C90 6.6, C99 6.8).

   statement:
     labeled-statement
     compound-statement
     expression-statement
     selection-statement
     iteration-statement
     jump-statement

   labeled-statement:
     label statement

   expression-statement:
     expression[opt] ;

   selection-statement:
     if-statement
     switch-statement

   iteration-statement:
     while-statement
     do-statement
     for-statement

   jump-statement:
     goto identifier ;
     continue ;
     break ;
     return expression[opt] ;

   GNU extensions:

   statement:
     asm-statement

   jump-statement:
     goto * expression ;

   Objective-C:

   statement:
     objc-throw-statement
     objc-try-catch-statement
     objc-synchronized-statement

   objc-throw-statement:
     @throw expression ;
     @throw ;

   OpenMP:

   statement:
     openmp-construct

   openmp-construct:
     parallel-construct
     for-construct
     simd-construct
     for-simd-construct
     sections-construct
     single-construct
     parallel-for-construct
     parallel-for-simd-construct
     parallel-sections-construct
     master-construct
     critical-construct
     atomic-construct
     ordered-construct

   parallel-construct:
     parallel-directive structured-block

   for-construct:
     for-directive iteration-statement

   simd-construct:
     simd-directive iteration-statements

   for-simd-construct:
     for-simd-directive iteration-statements

   sections-construct:
     sections-directive section-scope

   single-construct:
     single-directive structured-block

   parallel-for-construct:
     parallel-for-directive iteration-statement

   parallel-for-simd-construct:
     parallel-for-simd-directive iteration-statement

   parallel-sections-construct:
     parallel-sections-directive section-scope

   master-construct:
     master-directive structured-block

   critical-construct:
     critical-directive structured-block

   atomic-construct:
     atomic-directive expression-statement

   ordered-construct:
     ordered-directive structured-block

   Transactional Memory:

   statement:
     transaction-statement
     transaction-cancel-statement
*/

static void
c_parser_statement (c_parser *parser)
{
  c_parser_all_labels (parser);
  c_parser_statement_after_labels (parser);
}

/* Parse a statement, other than a labeled statement.  */

static void
c_parser_statement_after_labels (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  tree stmt = NULL_TREE;
  bool in_if_block = parser->in_if_block;
  parser->in_if_block = false;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_OPEN_BRACE:
      add_stmt (c_parser_compound_statement (parser));
      break;
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_IF:
	  c_parser_if_statement (parser);
	  break;
	case RID_SWITCH:
	  c_parser_switch_statement (parser);
	  break;
	case RID_WHILE:
	  c_parser_while_statement (parser, false);
	  break;
	case RID_DO:
	  c_parser_do_statement (parser, false);
	  break;
	case RID_FOR:
	  c_parser_for_statement (parser, false);
	  break;
	case RID_CILK_FOR:
	  if (!flag_cilkplus)
	    {
	      error_at (c_parser_peek_token (parser)->location,
			"-fcilkplus must be enabled to use %<_Cilk_for%>");
	      c_parser_skip_to_end_of_block_or_statement (parser);
	    }
	  else
	    c_parser_cilk_for (parser, integer_zero_node);
	  break;
	case RID_CILK_SYNC:
	  c_parser_consume_token (parser);
	  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
	  if (!flag_cilkplus) 
	    error_at (loc, "-fcilkplus must be enabled to use %<_Cilk_sync%>");
	  else 
	    add_stmt (build_cilk_sync ());
	  break;
	case RID_GOTO:
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    {
	      stmt = c_finish_goto_label (loc,
					  c_parser_peek_token (parser)->value);
	      c_parser_consume_token (parser);
	    }
	  else if (c_parser_next_token_is (parser, CPP_MULT))
	    {
	      struct c_expr val;

	      c_parser_consume_token (parser);
	      val = c_parser_expression (parser);
	      if (check_no_cilk (val.value,
				 "Cilk array notation cannot be used as a computed goto expression",
				 "%<_Cilk_spawn%> statement cannot be used as a computed goto expression",
				 loc))
	        val.value = error_mark_node;
	      val = convert_lvalue_to_rvalue (loc, val, false, true);
	      stmt = c_finish_goto_ptr (loc, val.value);
	    }
	  else
	    c_parser_error (parser, "expected identifier or %<*%>");
	  goto expect_semicolon;
	case RID_CONTINUE:
	  c_parser_consume_token (parser);
	  stmt = c_finish_bc_stmt (loc, &c_cont_label, false);
	  goto expect_semicolon;
	case RID_BREAK:
	  c_parser_consume_token (parser);
	  stmt = c_finish_bc_stmt (loc, &c_break_label, true);
	  goto expect_semicolon;
	case RID_RETURN:
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      stmt = c_finish_return (loc, NULL_TREE, NULL_TREE);
	      c_parser_consume_token (parser);
	    }
	  else
	    {
	      location_t xloc = c_parser_peek_token (parser)->location;
	      struct c_expr expr = c_parser_expression_conv (parser);
	      mark_exp_read (expr.value);
	      stmt = c_finish_return (xloc, expr.value, expr.original_type);
	      goto expect_semicolon;
	    }
	  break;
	case RID_ASM:
	  stmt = c_parser_asm_statement (parser);
	  break;
	case RID_TRANSACTION_ATOMIC:
	case RID_TRANSACTION_RELAXED:
	  stmt = c_parser_transaction (parser,
	      c_parser_peek_token (parser)->keyword);
	  break;
	case RID_TRANSACTION_CANCEL:
	  stmt = c_parser_transaction_cancel (parser);
	  goto expect_semicolon;
	case RID_AT_THROW:
	  gcc_assert (c_dialect_objc ());
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      stmt = objc_build_throw_stmt (loc, NULL_TREE);
	      c_parser_consume_token (parser);
	    }
	  else
	    {
	      struct c_expr expr = c_parser_expression (parser);
	      expr = convert_lvalue_to_rvalue (loc, expr, false, false);
	      if (check_no_cilk (expr.value,
		 "Cilk array notation cannot be used for a throw expression",
		 "%<_Cilk_spawn%> statement cannot be used for a throw expression"))
	        expr.value = error_mark_node;
	      else
		{
	          expr.value = c_fully_fold (expr.value, false, NULL);
	          stmt = objc_build_throw_stmt (loc, expr.value);
		}
	      goto expect_semicolon;
	    }
	  break;
	case RID_AT_TRY:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_try_catch_finally_statement (parser);
	  break;
	case RID_AT_SYNCHRONIZED:
	  gcc_assert (c_dialect_objc ());
	  c_parser_objc_synchronized_statement (parser);
	  break;
	default:
	  goto expr_stmt;
	}
      break;
    case CPP_SEMICOLON:
      c_parser_consume_token (parser);
      break;
    case CPP_CLOSE_PAREN:
    case CPP_CLOSE_SQUARE:
      /* Avoid infinite loop in error recovery:
	 c_parser_skip_until_found stops at a closing nesting
	 delimiter without consuming it, but here we need to consume
	 it to proceed further.  */
      c_parser_error (parser, "expected statement");
      c_parser_consume_token (parser);
      break;
    case CPP_PRAGMA:
      c_parser_pragma (parser, pragma_stmt);
      break;
    default:
    expr_stmt:
      stmt = c_finish_expr_stmt (loc, c_parser_expression_conv (parser).value);
    expect_semicolon:
      c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
      break;
    }
  /* Two cases cannot and do not have line numbers associated: If stmt
     is degenerate, such as "2;", then stmt is an INTEGER_CST, which
     cannot hold line numbers.  But that's OK because the statement
     will either be changed to a MODIFY_EXPR during gimplification of
     the statement expr, or discarded.  If stmt was compound, but
     without new variables, we will have skipped the creation of a
     BIND and will have a bare STATEMENT_LIST.  But that's OK because
     (recursively) all of the component statements should already have
     line numbers assigned.  ??? Can we discard no-op statements
     earlier?  */
  if (CAN_HAVE_LOCATION_P (stmt)
      && EXPR_LOCATION (stmt) == UNKNOWN_LOCATION)
    SET_EXPR_LOCATION (stmt, loc);

  parser->in_if_block = in_if_block;
}

/* Parse the condition from an if, do, while or for statements.  */

static tree
c_parser_condition (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  tree cond;
  cond = c_parser_expression_conv (parser).value;
  cond = c_objc_common_truthvalue_conversion (loc, cond);
  cond = c_fully_fold (cond, false, NULL);
  if (warn_sequence_point)
    verify_sequence_points (cond);
  return cond;
}

/* Parse a parenthesized condition from an if, do or while statement.

   condition:
     ( expression )
*/
static tree
c_parser_paren_condition (c_parser *parser)
{
  tree cond;
  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return error_mark_node;
  cond = c_parser_condition (parser);
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return cond;
}

/* Parse a statement which is a block in C99.  */

static tree
c_parser_c99_block_statement (c_parser *parser)
{
  tree block = c_begin_compound_stmt (flag_isoc99);
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_statement (parser);
  return c_end_compound_stmt (loc, block, flag_isoc99);
}

/* Parse the body of an if statement.  This is just parsing a
   statement but (a) it is a block in C99, (b) we track whether the
   body is an if statement for the sake of -Wparentheses warnings, (c)
   we handle an empty body specially for the sake of -Wempty-body
   warnings, and (d) we call parser_compound_statement directly
   because c_parser_statement_after_labels resets
   parser->in_if_block.  */

static tree
c_parser_if_body (c_parser *parser, bool *if_p)
{
  tree block = c_begin_compound_stmt (flag_isoc99);
  location_t body_loc = c_parser_peek_token (parser)->location;
  c_parser_all_labels (parser);
  *if_p = c_parser_next_token_is_keyword (parser, RID_IF);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      location_t loc = c_parser_peek_token (parser)->location;
      add_stmt (build_empty_stmt (loc));
      c_parser_consume_token (parser);
      if (!c_parser_next_token_is_keyword (parser, RID_ELSE))
	warning_at (loc, OPT_Wempty_body,
		    "suggest braces around empty body in an %<if%> statement");
    }
  else if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    add_stmt (c_parser_compound_statement (parser));
  else
    c_parser_statement_after_labels (parser);
  return c_end_compound_stmt (body_loc, block, flag_isoc99);
}

/* Parse the else body of an if statement.  This is just parsing a
   statement but (a) it is a block in C99, (b) we handle an empty body
   specially for the sake of -Wempty-body warnings.  */

static tree
c_parser_else_body (c_parser *parser)
{
  location_t else_loc = c_parser_peek_token (parser)->location;
  tree block = c_begin_compound_stmt (flag_isoc99);
  c_parser_all_labels (parser);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      location_t loc = c_parser_peek_token (parser)->location;
      warning_at (loc,
		  OPT_Wempty_body,
	         "suggest braces around empty body in an %<else%> statement");
      add_stmt (build_empty_stmt (loc));
      c_parser_consume_token (parser);
    }
  else
    c_parser_statement_after_labels (parser);
  return c_end_compound_stmt (else_loc, block, flag_isoc99);
}

/* Parse an if statement (C90 6.6.4, C99 6.8.4).

   if-statement:
     if ( expression ) statement
     if ( expression ) statement else statement
*/

static void
c_parser_if_statement (c_parser *parser)
{
  tree block;
  location_t loc;
  tree cond;
  bool first_if = false;
  tree first_body, second_body;
  bool in_if_block;
  tree if_stmt;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_IF));
  c_parser_consume_token (parser);
  block = c_begin_compound_stmt (flag_isoc99);
  loc = c_parser_peek_token (parser)->location;
  cond = c_parser_paren_condition (parser);
  if (flag_cilkplus && contains_cilk_spawn_stmt (cond))
    {
      error_at (loc, "if statement cannot contain %<Cilk_spawn%>");
      cond = error_mark_node;
    }
  in_if_block = parser->in_if_block;
  parser->in_if_block = true;
  first_body = c_parser_if_body (parser, &first_if);
  parser->in_if_block = in_if_block;
  if (c_parser_next_token_is_keyword (parser, RID_ELSE))
    {
      c_parser_consume_token (parser);
      second_body = c_parser_else_body (parser);
    }
  else
    second_body = NULL_TREE;
  c_finish_if_stmt (loc, cond, first_body, second_body, first_if);
  if_stmt = c_end_compound_stmt (loc, block, flag_isoc99);

  /* If the if statement contains array notations, then we expand them.  */
  if (flag_cilkplus && contains_array_notation_expr (if_stmt))
    if_stmt = fix_conditional_array_notations (if_stmt);
  add_stmt (if_stmt);
}

/* Parse a switch statement (C90 6.6.4, C99 6.8.4).

   switch-statement:
     switch (expression) statement
*/

static void
c_parser_switch_statement (c_parser *parser)
{
  struct c_expr ce;
  tree block, expr, body, save_break;
  location_t switch_loc = c_parser_peek_token (parser)->location;
  location_t switch_cond_loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_SWITCH));
  c_parser_consume_token (parser);
  block = c_begin_compound_stmt (flag_isoc99);
  bool explicit_cast_p = false;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      switch_cond_loc = c_parser_peek_token (parser)->location;
      if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
	  && c_token_starts_typename (c_parser_peek_2nd_token (parser)))
	explicit_cast_p = true;
      ce = c_parser_expression (parser);
      ce = convert_lvalue_to_rvalue (switch_cond_loc, ce, true, false);
      expr = ce.value;
      /* ??? expr has no valid location?  */
      if (check_no_cilk (expr,
	 "Cilk array notation cannot be used as a condition for switch statement",
	 "%<_Cilk_spawn%> statement cannot be used as a condition for switch statement",
			 switch_cond_loc))
        expr = error_mark_node;
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  else
    {
      switch_cond_loc = UNKNOWN_LOCATION;
      expr = error_mark_node;
    }
  c_start_case (switch_loc, switch_cond_loc, expr, explicit_cast_p);
  save_break = c_break_label;
  c_break_label = NULL_TREE;
  body = c_parser_c99_block_statement (parser);
  c_finish_case (body, ce.original_type);
  if (c_break_label)
    {
      location_t here = c_parser_peek_token (parser)->location;
      tree t = build1 (LABEL_EXPR, void_type_node, c_break_label);
      SET_EXPR_LOCATION (t, here);
      add_stmt (t);
    }
  c_break_label = save_break;
  add_stmt (c_end_compound_stmt (switch_loc, block, flag_isoc99));
}

/* Parse a while statement (C90 6.6.5, C99 6.8.5).

   while-statement:
      while (expression) statement
*/

static void
c_parser_while_statement (c_parser *parser, bool ivdep)
{
  tree block, cond, body, save_break, save_cont;
  location_t loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_WHILE));
  c_parser_consume_token (parser);
  block = c_begin_compound_stmt (flag_isoc99);
  loc = c_parser_peek_token (parser)->location;
  cond = c_parser_paren_condition (parser);
  if (check_no_cilk (cond,
         "Cilk array notation cannot be used as a condition for while statement",
	 "%<_Cilk_spawn%> statement cannot be used as a condition for while statement"))
    cond = error_mark_node;
  if (ivdep && cond != error_mark_node)
    cond = build2 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
		   build_int_cst (integer_type_node,
		   annot_expr_ivdep_kind));
  save_break = c_break_label;
  c_break_label = NULL_TREE;
  save_cont = c_cont_label;
  c_cont_label = NULL_TREE;
  body = c_parser_c99_block_statement (parser);
  c_finish_loop (loc, cond, NULL, body, c_break_label, c_cont_label, true);
  add_stmt (c_end_compound_stmt (loc, block, flag_isoc99));
  c_break_label = save_break;
  c_cont_label = save_cont;
}

/* Parse a do statement (C90 6.6.5, C99 6.8.5).

   do-statement:
     do statement while ( expression ) ;
*/

static void
c_parser_do_statement (c_parser *parser, bool ivdep)
{
  tree block, cond, body, save_break, save_cont, new_break, new_cont;
  location_t loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_DO));
  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    warning_at (c_parser_peek_token (parser)->location,
		OPT_Wempty_body,
		"suggest braces around empty body in %<do%> statement");
  block = c_begin_compound_stmt (flag_isoc99);
  loc = c_parser_peek_token (parser)->location;
  save_break = c_break_label;
  c_break_label = NULL_TREE;
  save_cont = c_cont_label;
  c_cont_label = NULL_TREE;
  body = c_parser_c99_block_statement (parser);
  c_parser_require_keyword (parser, RID_WHILE, "expected %<while%>");
  new_break = c_break_label;
  c_break_label = save_break;
  new_cont = c_cont_label;
  c_cont_label = save_cont;
  cond = c_parser_paren_condition (parser);
  if (check_no_cilk (cond,
	 "Cilk array notation cannot be used as a condition for a do-while statement",
	 "%<_Cilk_spawn%> statement cannot be used as a condition for a do-while statement"))
    cond = error_mark_node;
  if (ivdep && cond != error_mark_node)
    cond = build2 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
		   build_int_cst (integer_type_node,
		   annot_expr_ivdep_kind));
  if (!c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>"))
    c_parser_skip_to_end_of_block_or_statement (parser);
  c_finish_loop (loc, cond, NULL, body, new_break, new_cont, false);
  add_stmt (c_end_compound_stmt (loc, block, flag_isoc99));
}

/* Parse a for statement (C90 6.6.5, C99 6.8.5).

   for-statement:
     for ( expression[opt] ; expression[opt] ; expression[opt] ) statement
     for ( nested-declaration expression[opt] ; expression[opt] ) statement

   The form with a declaration is new in C99.

   ??? In accordance with the old parser, the declaration may be a
   nested function, which is then rejected in check_for_loop_decls,
   but does it make any sense for this to be included in the grammar?
   Note in particular that the nested function does not include a
   trailing ';', whereas the "declaration" production includes one.
   Also, can we reject bad declarations earlier and cheaper than
   check_for_loop_decls?

   In Objective-C, there are two additional variants:

   foreach-statement:
     for ( expression in expresssion ) statement
     for ( declaration in expression ) statement

   This is inconsistent with C, because the second variant is allowed
   even if c99 is not enabled.

   The rest of the comment documents these Objective-C foreach-statement.

   Here is the canonical example of the first variant:
    for (object in array)    { do something with object }
   we call the first expression ("object") the "object_expression" and 
   the second expression ("array") the "collection_expression".
   object_expression must be an lvalue of type "id" (a generic Objective-C
   object) because the loop works by assigning to object_expression the
   various objects from the collection_expression.  collection_expression
   must evaluate to something of type "id" which responds to the method
   countByEnumeratingWithState:objects:count:.

   The canonical example of the second variant is:
    for (id object in array)    { do something with object }
   which is completely equivalent to
    {
      id object;
      for (object in array) { do something with object }
    }
   Note that initizializing 'object' in some way (eg, "for ((object =
   xxx) in array) { do something with object }") is possibly
   technically valid, but completely pointless as 'object' will be
   assigned to something else as soon as the loop starts.  We should
   most likely reject it (TODO).

   The beginning of the Objective-C foreach-statement looks exactly
   like the beginning of the for-statement, and we can tell it is a
   foreach-statement only because the initial declaration or
   expression is terminated by 'in' instead of ';'.
*/

static void
c_parser_for_statement (c_parser *parser, bool ivdep)
{
  tree block, cond, incr, save_break, save_cont, body;
  /* The following are only used when parsing an ObjC foreach statement.  */
  tree object_expression;
  /* Silence the bogus uninitialized warning.  */
  tree collection_expression = NULL;
  location_t loc = c_parser_peek_token (parser)->location;
  location_t for_loc = c_parser_peek_token (parser)->location;
  bool is_foreach_statement = false;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_FOR));
  c_parser_consume_token (parser);
  /* Open a compound statement in Objective-C as well, just in case this is
     as foreach expression.  */
  block = c_begin_compound_stmt (flag_isoc99 || c_dialect_objc ());
  cond = error_mark_node;
  incr = error_mark_node;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      /* Parse the initialization declaration or expression.  */
      object_expression = error_mark_node;
      parser->objc_could_be_foreach_context = c_dialect_objc ();
      if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	{
	  parser->objc_could_be_foreach_context = false;
	  c_parser_consume_token (parser);
	  c_finish_expr_stmt (loc, NULL_TREE);
	}
      else if (c_parser_next_tokens_start_declaration (parser))
	{
	  c_parser_declaration_or_fndef (parser, true, true, true, true, true, 
					 &object_expression, vNULL);
	  parser->objc_could_be_foreach_context = false;
	  
	  if (c_parser_next_token_is_keyword (parser, RID_IN))
	    {
	      c_parser_consume_token (parser);
	      is_foreach_statement = true;
	      if (check_for_loop_decls (for_loc, true) == NULL_TREE)
		c_parser_error (parser, "multiple iterating variables in fast enumeration");
	    }
	  else
	    check_for_loop_decls (for_loc, flag_isoc99);
	}
      else if (c_parser_next_token_is_keyword (parser, RID_EXTENSION))
	{
	  /* __extension__ can start a declaration, but is also an
	     unary operator that can start an expression.  Consume all
	     but the last of a possible series of __extension__ to
	     determine which.  */
	  while (c_parser_peek_2nd_token (parser)->type == CPP_KEYWORD
		 && (c_parser_peek_2nd_token (parser)->keyword
		     == RID_EXTENSION))
	    c_parser_consume_token (parser);
	  if (c_token_starts_declaration (c_parser_peek_2nd_token (parser)))
	    {
	      int ext;
	      ext = disable_extension_diagnostics ();
	      c_parser_consume_token (parser);
	      c_parser_declaration_or_fndef (parser, true, true, true, true,
					     true, &object_expression, vNULL);
	      parser->objc_could_be_foreach_context = false;
	      
	      restore_extension_diagnostics (ext);
	      if (c_parser_next_token_is_keyword (parser, RID_IN))
		{
		  c_parser_consume_token (parser);
		  is_foreach_statement = true;
		  if (check_for_loop_decls (for_loc, true) == NULL_TREE)
		    c_parser_error (parser, "multiple iterating variables in fast enumeration");
		}
	      else
		check_for_loop_decls (for_loc, flag_isoc99);
	    }
	  else
	    goto init_expr;
	}
      else
	{
	init_expr:
	  {
	    struct c_expr ce;
	    tree init_expression;
	    ce = c_parser_expression (parser);
	    /* In theory we could forbid _Cilk_spawn here, as the spec says "only in top
	       level statement", but it works just fine, so allow it.  */
	    init_expression = ce.value;
	    parser->objc_could_be_foreach_context = false;
	    if (c_parser_next_token_is_keyword (parser, RID_IN))
	      {
		c_parser_consume_token (parser);
		is_foreach_statement = true;
		if (! lvalue_p (init_expression))
		  c_parser_error (parser, "invalid iterating variable in fast enumeration");
		object_expression = c_fully_fold (init_expression, false, NULL);
	      }
	    else
	      {
		ce = convert_lvalue_to_rvalue (loc, ce, true, false);
		init_expression = ce.value;
		c_finish_expr_stmt (loc, init_expression);
		c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
	      }
	  }
	}
      /* Parse the loop condition.  In the case of a foreach
	 statement, there is no loop condition.  */
      gcc_assert (!parser->objc_could_be_foreach_context);
      if (!is_foreach_statement)
	{
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      if (ivdep)
		{
		  c_parser_error (parser, "missing loop condition in loop with "
				  "%<GCC ivdep%> pragma");
		  cond = error_mark_node;
		}
	      else
		{
		  c_parser_consume_token (parser);
		  cond = NULL_TREE;
		}
	    }
	  else
	    {
	      cond = c_parser_condition (parser);
	      if (check_no_cilk (cond,
		 "Cilk array notation cannot be used in a condition for a for-loop",
		 "%<_Cilk_spawn%> statement cannot be used in a condition for a for-loop"))
		cond = error_mark_node;
	      c_parser_skip_until_found (parser, CPP_SEMICOLON,
					 "expected %<;%>");
	    }
	  if (ivdep && cond != error_mark_node)
	    cond = build2 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
			   build_int_cst (integer_type_node,
			   annot_expr_ivdep_kind));
	}
      /* Parse the increment expression (the third expression in a
	 for-statement).  In the case of a foreach-statement, this is
	 the expression that follows the 'in'.  */
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  if (is_foreach_statement)
	    {
	      c_parser_error (parser, "missing collection in fast enumeration");
	      collection_expression = error_mark_node;
	    }
	  else
	    incr = c_process_expr_stmt (loc, NULL_TREE);
	}
      else
	{
	  if (is_foreach_statement)
	    collection_expression = c_fully_fold (c_parser_expression (parser).value,
						  false, NULL);
	  else
	    {
	      struct c_expr ce = c_parser_expression (parser);
	      ce = convert_lvalue_to_rvalue (loc, ce, true, false);
	      incr = c_process_expr_stmt (loc, ce.value);
	    }
	}
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  save_break = c_break_label;
  c_break_label = NULL_TREE;
  save_cont = c_cont_label;
  c_cont_label = NULL_TREE;
  body = c_parser_c99_block_statement (parser);
  if (is_foreach_statement)
    objc_finish_foreach_loop (loc, object_expression, collection_expression, body, c_break_label, c_cont_label);
  else
    c_finish_loop (loc, cond, incr, body, c_break_label, c_cont_label, true);
  add_stmt (c_end_compound_stmt (loc, block, flag_isoc99 || c_dialect_objc ()));
  c_break_label = save_break;
  c_cont_label = save_cont;
}

/* Parse an asm statement, a GNU extension.  This is a full-blown asm
   statement with inputs, outputs, clobbers, and volatile tag
   allowed.

   asm-statement:
     asm type-qualifier[opt] ( asm-argument ) ;
     asm type-qualifier[opt] goto ( asm-goto-argument ) ;

   asm-argument:
     asm-string-literal
     asm-string-literal : asm-operands[opt]
     asm-string-literal : asm-operands[opt] : asm-operands[opt]
     asm-string-literal : asm-operands[opt] : asm-operands[opt] : asm-clobbers[opt]

   asm-goto-argument:
     asm-string-literal : : asm-operands[opt] : asm-clobbers[opt] \
       : asm-goto-operands

   Qualifiers other than volatile are accepted in the syntax but
   warned for.  */

static tree
c_parser_asm_statement (c_parser *parser)
{
  tree quals, str, outputs, inputs, clobbers, labels, ret;
  bool simple, is_goto;
  location_t asm_loc = c_parser_peek_token (parser)->location;
  int section, nsections;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ASM));
  c_parser_consume_token (parser);
  if (c_parser_next_token_is_keyword (parser, RID_VOLATILE))
    {
      quals = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
    }
  else if (c_parser_next_token_is_keyword (parser, RID_CONST)
	   || c_parser_next_token_is_keyword (parser, RID_RESTRICT))
    {
      warning_at (c_parser_peek_token (parser)->location,
		  0,
		  "%E qualifier ignored on asm",
		  c_parser_peek_token (parser)->value);
      quals = NULL_TREE;
      c_parser_consume_token (parser);
    }
  else
    quals = NULL_TREE;

  is_goto = false;
  if (c_parser_next_token_is_keyword (parser, RID_GOTO))
    {
      c_parser_consume_token (parser);
      is_goto = true;
    }

  /* ??? Follow the C++ parser rather than using the
     lex_untranslated_string kludge.  */
  parser->lex_untranslated_string = true;
  ret = NULL;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    goto error;

  str = c_parser_asm_string_literal (parser);
  if (str == NULL_TREE)
    goto error_close_paren;

  simple = true;
  outputs = NULL_TREE;
  inputs = NULL_TREE;
  clobbers = NULL_TREE;
  labels = NULL_TREE;

  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN) && !is_goto)
    goto done_asm;

  /* Parse each colon-delimited section of operands.  */
  nsections = 3 + is_goto;
  for (section = 0; section < nsections; ++section)
    {
      if (!c_parser_require (parser, CPP_COLON,
			     is_goto
			     ? "expected %<:%>"
			     : "expected %<:%> or %<)%>"))
	goto error_close_paren;

      /* Once past any colon, we're no longer a simple asm.  */
      simple = false;

      if ((!c_parser_next_token_is (parser, CPP_COLON)
	   && !c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	  || section == 3)
	switch (section)
	  {
	  case 0:
	    /* For asm goto, we don't allow output operands, but reserve
	       the slot for a future extension that does allow them.  */
	    if (!is_goto)
	      outputs = c_parser_asm_operands (parser);
	    break;
	  case 1:
	    inputs = c_parser_asm_operands (parser);
	    break;
	  case 2:
	    clobbers = c_parser_asm_clobbers (parser);
	    break;
	  case 3:
	    labels = c_parser_asm_goto_operands (parser);
	    break;
	  default:
	    gcc_unreachable ();
	  }

      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN) && !is_goto)
	goto done_asm;
    }

 done_asm:
  if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      goto error;
    }

  if (!c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>"))
    c_parser_skip_to_end_of_block_or_statement (parser);

  ret = build_asm_stmt (quals, build_asm_expr (asm_loc, str, outputs, inputs,
					       clobbers, labels, simple));

 error:
  parser->lex_untranslated_string = false;
  return ret;

 error_close_paren:
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
  goto error;
}

/* Parse asm operands, a GNU extension.

   asm-operands:
     asm-operand
     asm-operands , asm-operand

   asm-operand:
     asm-string-literal ( expression )
     [ identifier ] asm-string-literal ( expression )
*/

static tree
c_parser_asm_operands (c_parser *parser)
{
  tree list = NULL_TREE;
  while (true)
    {
      tree name, str;
      struct c_expr expr;
      if (c_parser_next_token_is (parser, CPP_OPEN_SQUARE))
	{
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    {
	      tree id = c_parser_peek_token (parser)->value;
	      c_parser_consume_token (parser);
	      name = build_string (IDENTIFIER_LENGTH (id),
				   IDENTIFIER_POINTER (id));
	    }
	  else
	    {
	      c_parser_error (parser, "expected identifier");
	      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	      return NULL_TREE;
	    }
	  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
				     "expected %<]%>");
	}
      else
	name = NULL_TREE;
      str = c_parser_asm_string_literal (parser);
      if (str == NULL_TREE)
	return NULL_TREE;
      parser->lex_untranslated_string = false;
      if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	{
	  parser->lex_untranslated_string = true;
	  return NULL_TREE;
	}
      expr = c_parser_expression (parser);
      mark_exp_read (expr.value);
      parser->lex_untranslated_string = true;
      if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return NULL_TREE;
	}
      list = chainon (list, build_tree_list (build_tree_list (name, str),
					     expr.value));
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  return list;
}

/* Parse asm clobbers, a GNU extension.

   asm-clobbers:
     asm-string-literal
     asm-clobbers , asm-string-literal
*/

static tree
c_parser_asm_clobbers (c_parser *parser)
{
  tree list = NULL_TREE;
  while (true)
    {
      tree str = c_parser_asm_string_literal (parser);
      if (str)
	list = tree_cons (NULL_TREE, str, list);
      else
	return NULL_TREE;
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  return list;
}

/* Parse asm goto labels, a GNU extension.

   asm-goto-operands:
     identifier
     asm-goto-operands , identifier
*/

static tree
c_parser_asm_goto_operands (c_parser *parser)
{
  tree list = NULL_TREE;
  while (true)
    {
      tree name, label;

      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  c_token *tok = c_parser_peek_token (parser);
	  name = tok->value;
	  label = lookup_label_for_goto (tok->location, name);
	  c_parser_consume_token (parser);
	  TREE_USED (label) = 1;
	}
      else
	{
	  c_parser_error (parser, "expected identifier");
	  return NULL_TREE;
	}

      name = build_string (IDENTIFIER_LENGTH (name),
			   IDENTIFIER_POINTER (name));
      list = tree_cons (name, label, list);
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	return nreverse (list);
    }
}

/* Parse an expression other than a compound expression; that is, an
   assignment expression (C90 6.3.16, C99 6.5.16).  If AFTER is not
   NULL then it is an Objective-C message expression which is the
   primary-expression starting the expression as an initializer.

   assignment-expression:
     conditional-expression
     unary-expression assignment-operator assignment-expression

   assignment-operator: one of
     = *= /= %= += -= <<= >>= &= ^= |=

   In GNU C we accept any conditional expression on the LHS and
   diagnose the invalid lvalue rather than producing a syntax
   error.  */

static struct c_expr
c_parser_expr_no_commas (c_parser *parser, struct c_expr *after,
			 tree omp_atomic_lhs)
{
  struct c_expr lhs, rhs, ret;
  enum tree_code code;
  location_t op_location, exp_location;
  gcc_assert (!after || c_dialect_objc ());
  lhs = c_parser_conditional_expression (parser, after, omp_atomic_lhs);
  op_location = c_parser_peek_token (parser)->location;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_EQ:
      code = NOP_EXPR;
      break;
    case CPP_MULT_EQ:
      code = MULT_EXPR;
      break;
    case CPP_DIV_EQ:
      code = TRUNC_DIV_EXPR;
      break;
    case CPP_MOD_EQ:
      code = TRUNC_MOD_EXPR;
      break;
    case CPP_PLUS_EQ:
      code = PLUS_EXPR;
      break;
    case CPP_MINUS_EQ:
      code = MINUS_EXPR;
      break;
    case CPP_LSHIFT_EQ:
      code = LSHIFT_EXPR;
      break;
    case CPP_RSHIFT_EQ:
      code = RSHIFT_EXPR;
      break;
    case CPP_AND_EQ:
      code = BIT_AND_EXPR;
      break;
    case CPP_XOR_EQ:
      code = BIT_XOR_EXPR;
      break;
    case CPP_OR_EQ:
      code = BIT_IOR_EXPR;
      break;
    default:
      return lhs;
    }
  c_parser_consume_token (parser);
  exp_location = c_parser_peek_token (parser)->location;
  rhs = c_parser_expr_no_commas (parser, NULL);
  rhs = convert_lvalue_to_rvalue (exp_location, rhs, true, true);
  
  ret.value = build_modify_expr (op_location, lhs.value, lhs.original_type,
				 code, exp_location, rhs.value,
				 rhs.original_type);
  if (code == NOP_EXPR)
    ret.original_code = MODIFY_EXPR;
  else
    {
      TREE_NO_WARNING (ret.value) = 1;
      ret.original_code = ERROR_MARK;
    }
  ret.original_type = NULL;
  return ret;
}

/* Parse a conditional expression (C90 6.3.15, C99 6.5.15).  If AFTER
   is not NULL then it is an Objective-C message expression which is
   the primary-expression starting the expression as an initializer.

   conditional-expression:
     logical-OR-expression
     logical-OR-expression ? expression : conditional-expression

   GNU extensions:

   conditional-expression:
     logical-OR-expression ? : conditional-expression
*/

static struct c_expr
c_parser_conditional_expression (c_parser *parser, struct c_expr *after,
				 tree omp_atomic_lhs)
{
  struct c_expr cond, exp1, exp2, ret;
  location_t cond_loc, colon_loc, middle_loc;

  gcc_assert (!after || c_dialect_objc ());

  cond = c_parser_binary_expression (parser, after, omp_atomic_lhs);

  if (c_parser_next_token_is_not (parser, CPP_QUERY))
    return cond;
  cond_loc = c_parser_peek_token (parser)->location;
  cond = convert_lvalue_to_rvalue (cond_loc, cond, true, true);
  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_COLON))
    {
      tree eptype = NULL_TREE;

      middle_loc = c_parser_peek_token (parser)->location;
      pedwarn (middle_loc, OPT_Wpedantic, 
	       "ISO C forbids omitting the middle term of a ?: expression");
      warn_for_omitted_condop (middle_loc, cond.value);
      if (TREE_CODE (cond.value) == EXCESS_PRECISION_EXPR)
	{
	  eptype = TREE_TYPE (cond.value);
	  cond.value = TREE_OPERAND (cond.value, 0);
	}
      /* Make sure first operand is calculated only once.  */
      exp1.value = c_save_expr (default_conversion (cond.value));
      if (eptype)
	exp1.value = build1 (EXCESS_PRECISION_EXPR, eptype, exp1.value);
      exp1.original_type = NULL;
      cond.value = c_objc_common_truthvalue_conversion (cond_loc, exp1.value);
      c_inhibit_evaluation_warnings += cond.value == truthvalue_true_node;
    }
  else
    {
      cond.value
	= c_objc_common_truthvalue_conversion
	(cond_loc, default_conversion (cond.value));
      c_inhibit_evaluation_warnings += cond.value == truthvalue_false_node;
      exp1 = c_parser_expression_conv (parser);
      mark_exp_read (exp1.value);
      c_inhibit_evaluation_warnings +=
	((cond.value == truthvalue_true_node)
	 - (cond.value == truthvalue_false_node));
    }

  colon_loc = c_parser_peek_token (parser)->location;
  if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
    {
      c_inhibit_evaluation_warnings -= cond.value == truthvalue_true_node;
      ret.value = error_mark_node;
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      return ret;
    }
  {
    location_t exp2_loc = c_parser_peek_token (parser)->location;
    exp2 = c_parser_conditional_expression (parser, NULL, NULL_TREE);
    exp2 = convert_lvalue_to_rvalue (exp2_loc, exp2, true, true);
  }
  c_inhibit_evaluation_warnings -= cond.value == truthvalue_true_node;
  ret.value = build_conditional_expr (colon_loc, cond.value,
				      cond.original_code == C_MAYBE_CONST_EXPR,
				      exp1.value, exp1.original_type,
				      exp2.value, exp2.original_type);
  ret.original_code = ERROR_MARK;
  if (exp1.value == error_mark_node || exp2.value == error_mark_node)
    ret.original_type = NULL;
  else
    {
      tree t1, t2;

      /* If both sides are enum type, the default conversion will have
	 made the type of the result be an integer type.  We want to
	 remember the enum types we started with.  */
      t1 = exp1.original_type ? exp1.original_type : TREE_TYPE (exp1.value);
      t2 = exp2.original_type ? exp2.original_type : TREE_TYPE (exp2.value);
      ret.original_type = ((t1 != error_mark_node
			    && t2 != error_mark_node
			    && (TYPE_MAIN_VARIANT (t1)
				== TYPE_MAIN_VARIANT (t2)))
			   ? t1
			   : NULL);
    }
  return ret;
}

/* Parse a binary expression; that is, a logical-OR-expression (C90
   6.3.5-6.3.14, C99 6.5.5-6.5.14).  If AFTER is not NULL then it is
   an Objective-C message expression which is the primary-expression
   starting the expression as an initializer.

   OMP_ATOMIC_LHS is NULL, unless parsing OpenMP #pragma omp atomic,
   when it should be the unfolded lhs.  In a valid OpenMP source,
   one of the operands of the toplevel binary expression must be equal
   to it.  In that case, just return a build2 created binary operation
   rather than result of parser_build_binary_op.

   multiplicative-expression:
     cast-expression
     multiplicative-expression * cast-expression
     multiplicative-expression / cast-expression
     multiplicative-expression % cast-expression

   additive-expression:
     multiplicative-expression
     additive-expression + multiplicative-expression
     additive-expression - multiplicative-expression

   shift-expression:
     additive-expression
     shift-expression << additive-expression
     shift-expression >> additive-expression

   relational-expression:
     shift-expression
     relational-expression < shift-expression
     relational-expression > shift-expression
     relational-expression <= shift-expression
     relational-expression >= shift-expression

   equality-expression:
     relational-expression
     equality-expression == relational-expression
     equality-expression != relational-expression

   AND-expression:
     equality-expression
     AND-expression & equality-expression

   exclusive-OR-expression:
     AND-expression
     exclusive-OR-expression ^ AND-expression

   inclusive-OR-expression:
     exclusive-OR-expression
     inclusive-OR-expression | exclusive-OR-expression

   logical-AND-expression:
     inclusive-OR-expression
     logical-AND-expression && inclusive-OR-expression

   logical-OR-expression:
     logical-AND-expression
     logical-OR-expression || logical-AND-expression
*/

static struct c_expr
c_parser_binary_expression (c_parser *parser, struct c_expr *after,
			    tree omp_atomic_lhs)
{
  /* A binary expression is parsed using operator-precedence parsing,
     with the operands being cast expressions.  All the binary
     operators are left-associative.  Thus a binary expression is of
     form:

     E0 op1 E1 op2 E2 ...

     which we represent on a stack.  On the stack, the precedence
     levels are strictly increasing.  When a new operator is
     encountered of higher precedence than that at the top of the
     stack, it is pushed; its LHS is the top expression, and its RHS
     is everything parsed until it is popped.  When a new operator is
     encountered with precedence less than or equal to that at the top
     of the stack, triples E[i-1] op[i] E[i] are popped and replaced
     by the result of the operation until the operator at the top of
     the stack has lower precedence than the new operator or there is
     only one element on the stack; then the top expression is the LHS
     of the new operator.  In the case of logical AND and OR
     expressions, we also need to adjust c_inhibit_evaluation_warnings
     as appropriate when the operators are pushed and popped.  */

  struct {
    /* The expression at this stack level.  */
    struct c_expr expr;
    /* The precedence of the operator on its left, PREC_NONE at the
       bottom of the stack.  */
    enum c_parser_prec prec;
    /* The operation on its left.  */
    enum tree_code op;
    /* The source location of this operation.  */
    location_t loc;
  } stack[NUM_PRECS];
  int sp;
  /* Location of the binary operator.  */
  location_t binary_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
#define POP								      \
  do {									      \
    switch (stack[sp].op)						      \
      {									      \
      case TRUTH_ANDIF_EXPR:						      \
	c_inhibit_evaluation_warnings -= (stack[sp - 1].expr.value	      \
					  == truthvalue_false_node);	      \
	break;								      \
      case TRUTH_ORIF_EXPR:						      \
	c_inhibit_evaluation_warnings -= (stack[sp - 1].expr.value	      \
					  == truthvalue_true_node);	      \
	break;								      \
      default:								      \
	break;								      \
      }									      \
    stack[sp - 1].expr							      \
      = convert_lvalue_to_rvalue (stack[sp - 1].loc,			      \
				  stack[sp - 1].expr, true, true);	      \
    stack[sp].expr							      \
      = convert_lvalue_to_rvalue (stack[sp].loc,			      \
				  stack[sp].expr, true, true);		      \
    if (__builtin_expect (omp_atomic_lhs != NULL_TREE, 0) && sp == 1	      \
	&& c_parser_peek_token (parser)->type == CPP_SEMICOLON		      \
	&& ((1 << stack[sp].prec)					      \
	    & (1 << (PREC_BITOR | PREC_BITXOR | PREC_BITAND | PREC_SHIFT      \
		     | PREC_ADD | PREC_MULT)))				      \
	&& stack[sp].op != TRUNC_MOD_EXPR				      \
	&& stack[0].expr.value != error_mark_node			      \
	&& stack[1].expr.value != error_mark_node			      \
	&& (c_tree_equal (stack[0].expr.value, omp_atomic_lhs)		      \
	    || c_tree_equal (stack[1].expr.value, omp_atomic_lhs)))	      \
      stack[0].expr.value						      \
	= build2 (stack[1].op, TREE_TYPE (stack[0].expr.value),		      \
		  stack[0].expr.value, stack[1].expr.value);		      \
    else								      \
      stack[sp - 1].expr = parser_build_binary_op (stack[sp].loc,	      \
						   stack[sp].op,	      \
						   stack[sp - 1].expr,	      \
						   stack[sp].expr);	      \
    sp--;								      \
  } while (0)
  gcc_assert (!after || c_dialect_objc ());
  stack[0].loc = c_parser_peek_token (parser)->location;
  stack[0].expr = c_parser_cast_expression (parser, after);
  stack[0].prec = PREC_NONE;
  sp = 0;
  while (true)
    {
      enum c_parser_prec oprec;
      enum tree_code ocode;
      if (parser->error)
	goto out;
      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_MULT:
	  oprec = PREC_MULT;
	  ocode = MULT_EXPR;
	  break;
	case CPP_DIV:
	  oprec = PREC_MULT;
	  ocode = TRUNC_DIV_EXPR;
	  break;
	case CPP_MOD:
	  oprec = PREC_MULT;
	  ocode = TRUNC_MOD_EXPR;
	  break;
	case CPP_PLUS:
	  oprec = PREC_ADD;
	  ocode = PLUS_EXPR;
	  break;
	case CPP_MINUS:
	  oprec = PREC_ADD;
	  ocode = MINUS_EXPR;
	  break;
	case CPP_LSHIFT:
	  oprec = PREC_SHIFT;
	  ocode = LSHIFT_EXPR;
	  break;
	case CPP_RSHIFT:
	  oprec = PREC_SHIFT;
	  ocode = RSHIFT_EXPR;
	  break;
	case CPP_LESS:
	  oprec = PREC_REL;
	  ocode = LT_EXPR;
	  break;
	case CPP_GREATER:
	  oprec = PREC_REL;
	  ocode = GT_EXPR;
	  break;
	case CPP_LESS_EQ:
	  oprec = PREC_REL;
	  ocode = LE_EXPR;
	  break;
	case CPP_GREATER_EQ:
	  oprec = PREC_REL;
	  ocode = GE_EXPR;
	  break;
	case CPP_EQ_EQ:
	  oprec = PREC_EQ;
	  ocode = EQ_EXPR;
	  break;
	case CPP_NOT_EQ:
	  oprec = PREC_EQ;
	  ocode = NE_EXPR;
	  break;
	case CPP_AND:
	  oprec = PREC_BITAND;
	  ocode = BIT_AND_EXPR;
	  break;
	case CPP_XOR:
	  oprec = PREC_BITXOR;
	  ocode = BIT_XOR_EXPR;
	  break;
	case CPP_OR:
	  oprec = PREC_BITOR;
	  ocode = BIT_IOR_EXPR;
	  break;
	case CPP_AND_AND:
	  oprec = PREC_LOGAND;
	  ocode = TRUTH_ANDIF_EXPR;
	  break;
	case CPP_OR_OR:
	  oprec = PREC_LOGOR;
	  ocode = TRUTH_ORIF_EXPR;
	  break;
	default:
	  /* Not a binary operator, so end of the binary
	     expression.  */
	  goto out;
	}
      binary_loc = c_parser_peek_token (parser)->location;
      while (oprec <= stack[sp].prec)
	POP;
      c_parser_consume_token (parser);
      switch (ocode)
	{
	case TRUTH_ANDIF_EXPR:
	  stack[sp].expr
	    = convert_lvalue_to_rvalue (stack[sp].loc,
					stack[sp].expr, true, true);
	  stack[sp].expr.value = c_objc_common_truthvalue_conversion
	    (stack[sp].loc, default_conversion (stack[sp].expr.value));
	  c_inhibit_evaluation_warnings += (stack[sp].expr.value
					    == truthvalue_false_node);
	  break;
	case TRUTH_ORIF_EXPR:
	  stack[sp].expr
	    = convert_lvalue_to_rvalue (stack[sp].loc,
					stack[sp].expr, true, true);
	  stack[sp].expr.value = c_objc_common_truthvalue_conversion
	    (stack[sp].loc, default_conversion (stack[sp].expr.value));
	  c_inhibit_evaluation_warnings += (stack[sp].expr.value
					    == truthvalue_true_node);
	  break;
	default:
	  break;
	}
      sp++;
      stack[sp].loc = binary_loc;
      stack[sp].expr = c_parser_cast_expression (parser, NULL);
      stack[sp].prec = oprec;
      stack[sp].op = ocode;
      stack[sp].loc = binary_loc;
    }
 out:
  while (sp > 0)
    POP;
  return stack[0].expr;
#undef POP
}

/* Parse a cast expression (C90 6.3.4, C99 6.5.4).  If AFTER is not
   NULL then it is an Objective-C message expression which is the
   primary-expression starting the expression as an initializer.

   cast-expression:
     unary-expression
     ( type-name ) unary-expression
*/

static struct c_expr
c_parser_cast_expression (c_parser *parser, struct c_expr *after)
{
  location_t cast_loc = c_parser_peek_token (parser)->location;
  gcc_assert (!after || c_dialect_objc ());
  if (after)
    return c_parser_postfix_expression_after_primary (parser,
						      cast_loc, *after);
  /* If the expression begins with a parenthesized type name, it may
     be either a cast or a compound literal; we need to see whether
     the next character is '{' to tell the difference.  If not, it is
     an unary expression.  Full detection of unknown typenames here
     would require a 3-token lookahead.  */
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
      && c_token_starts_typename (c_parser_peek_2nd_token (parser)))
    {
      struct c_type_name *type_name;
      struct c_expr ret;
      struct c_expr expr;
      c_parser_consume_token (parser);
      type_name = c_parser_type_name (parser);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
      if (type_name == NULL)
	{
	  ret.value = error_mark_node;
	  ret.original_code = ERROR_MARK;
	  ret.original_type = NULL;
	  return ret;
	}

      /* Save casted types in the function's used types hash table.  */
      used_types_insert (type_name->specs->type);

      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	return c_parser_postfix_expression_after_paren_type (parser, type_name,
							     cast_loc);
      {
	location_t expr_loc = c_parser_peek_token (parser)->location;
	expr = c_parser_cast_expression (parser, NULL);
	expr = convert_lvalue_to_rvalue (expr_loc, expr, true, true);
      }
      ret.value = c_cast_expr (cast_loc, type_name, expr.value);
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      return ret;
    }
  else
    return c_parser_unary_expression (parser);
}

/* Parse an unary expression (C90 6.3.3, C99 6.5.3).

   unary-expression:
     postfix-expression
     ++ unary-expression
     -- unary-expression
     unary-operator cast-expression
     sizeof unary-expression
     sizeof ( type-name )

   unary-operator: one of
     & * + - ~ !

   GNU extensions:

   unary-expression:
     __alignof__ unary-expression
     __alignof__ ( type-name )
     && identifier

   (C11 permits _Alignof with type names only.)

   unary-operator: one of
     __extension__ __real__ __imag__

   Transactional Memory:

   unary-expression:
     transaction-expression

   In addition, the GNU syntax treats ++ and -- as unary operators, so
   they may be applied to cast expressions with errors for non-lvalues
   given later.  */

static struct c_expr
c_parser_unary_expression (c_parser *parser)
{
  int ext;
  struct c_expr ret, op;
  location_t op_loc = c_parser_peek_token (parser)->location;
  location_t exp_loc;
  ret.original_code = ERROR_MARK;
  ret.original_type = NULL;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_PLUS_PLUS:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);

      /* If there is array notations in op, we expand them.  */
      if (flag_cilkplus && TREE_CODE (op.value) == ARRAY_NOTATION_REF)
	return fix_array_notation_expr (exp_loc, PREINCREMENT_EXPR, op);
      else
	{
	  op = default_function_array_read_conversion (exp_loc, op);
	  return parser_build_unary_op (op_loc, PREINCREMENT_EXPR, op);
	}
    case CPP_MINUS_MINUS:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      
      /* If there is array notations in op, we expand them.  */
      if (flag_cilkplus && TREE_CODE (op.value) == ARRAY_NOTATION_REF)
	return fix_array_notation_expr (exp_loc, PREDECREMENT_EXPR, op);
      else
	{
	  op = default_function_array_read_conversion (exp_loc, op);
	  return parser_build_unary_op (op_loc, PREDECREMENT_EXPR, op);
	}
    case CPP_AND:
      c_parser_consume_token (parser);
      op = c_parser_cast_expression (parser, NULL);
      mark_exp_read (op.value);
      return parser_build_unary_op (op_loc, ADDR_EXPR, op);
    case CPP_MULT:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      ret.value = build_indirect_ref (op_loc, op.value, RO_UNARY_STAR);
      return ret;
    case CPP_PLUS:
      if (!c_dialect_objc () && !in_system_header_at (input_location))
	warning_at (op_loc,
		    OPT_Wtraditional,
		    "traditional C rejects the unary plus operator");
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, CONVERT_EXPR, op);
    case CPP_MINUS:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, NEGATE_EXPR, op);
    case CPP_COMPL:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, BIT_NOT_EXPR, op);
    case CPP_NOT:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, TRUTH_NOT_EXPR, op);
    case CPP_AND_AND:
      /* Refer to the address of a label as a pointer.  */
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  ret.value = finish_label_address_expr
	    (c_parser_peek_token (parser)->value, op_loc);
	  c_parser_consume_token (parser);
	}
      else
	{
	  c_parser_error (parser, "expected identifier");
	  ret.value = error_mark_node;
	}
	return ret;
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_SIZEOF:
	  return c_parser_sizeof_expression (parser);
	case RID_ALIGNOF:
	  return c_parser_alignof_expression (parser);
	case RID_EXTENSION:
	  c_parser_consume_token (parser);
	  ext = disable_extension_diagnostics ();
	  ret = c_parser_cast_expression (parser, NULL);
	  restore_extension_diagnostics (ext);
	  return ret;
	case RID_REALPART:
	  c_parser_consume_token (parser);
	  exp_loc = c_parser_peek_token (parser)->location;
	  op = c_parser_cast_expression (parser, NULL);
	  op = default_function_array_conversion (exp_loc, op);
	  return parser_build_unary_op (op_loc, REALPART_EXPR, op);
	case RID_IMAGPART:
	  c_parser_consume_token (parser);
	  exp_loc = c_parser_peek_token (parser)->location;
	  op = c_parser_cast_expression (parser, NULL);
	  op = default_function_array_conversion (exp_loc, op);
	  return parser_build_unary_op (op_loc, IMAGPART_EXPR, op);
	case RID_TRANSACTION_ATOMIC:
	case RID_TRANSACTION_RELAXED:
	  return c_parser_transaction_expression (parser,
	      c_parser_peek_token (parser)->keyword);
	default:
	  return c_parser_postfix_expression (parser);
	}
    default:
      return c_parser_postfix_expression (parser);
    }
}

/* Parse a sizeof expression.  */

static struct c_expr
c_parser_sizeof_expression (c_parser *parser)
{
  struct c_expr expr;
  location_t expr_loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_SIZEOF));
  c_parser_consume_token (parser);
  c_inhibit_evaluation_warnings++;
  in_sizeof++;
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
      && c_token_starts_typename (c_parser_peek_2nd_token (parser)))
    {
      /* Either sizeof ( type-name ) or sizeof unary-expression
	 starting with a compound literal.  */
      struct c_type_name *type_name;
      c_parser_consume_token (parser);
      expr_loc = c_parser_peek_token (parser)->location;
      type_name = c_parser_type_name (parser);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
      if (type_name == NULL)
	{
	  struct c_expr ret;
	  c_inhibit_evaluation_warnings--;
	  in_sizeof--;
	  ret.value = error_mark_node;
	  ret.original_code = ERROR_MARK;
	  ret.original_type = NULL;
	  return ret;
	}
      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	{
	  expr = c_parser_postfix_expression_after_paren_type (parser,
							       type_name,
							       expr_loc);
	  goto sizeof_expr;
	}
      /* sizeof ( type-name ).  */
      c_inhibit_evaluation_warnings--;
      in_sizeof--;
      return c_expr_sizeof_type (expr_loc, type_name);
    }
  else
    {
      expr_loc = c_parser_peek_token (parser)->location;
      expr = c_parser_unary_expression (parser);
    sizeof_expr:
      c_inhibit_evaluation_warnings--;
      in_sizeof--;
      mark_exp_read (expr.value);
      if (TREE_CODE (expr.value) == COMPONENT_REF
	  && DECL_C_BIT_FIELD (TREE_OPERAND (expr.value, 1)))
	error_at (expr_loc, "%<sizeof%> applied to a bit-field");
      return c_expr_sizeof_expr (expr_loc, expr);
    }
}

/* Parse an alignof expression.  */

static struct c_expr
c_parser_alignof_expression (c_parser *parser)
{
  struct c_expr expr;
  location_t loc = c_parser_peek_token (parser)->location;
  tree alignof_spelling = c_parser_peek_token (parser)->value;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ALIGNOF));
  bool is_c11_alignof = strcmp (IDENTIFIER_POINTER (alignof_spelling),
				"_Alignof") == 0;
  /* A diagnostic is not required for the use of this identifier in
     the implementation namespace; only diagnose it for the C11
     spelling because of existing code using the other spellings.  */
  if (is_c11_alignof)
    {
      if (flag_isoc99)
	pedwarn_c99 (loc, OPT_Wpedantic, "ISO C99 does not support %qE",
		     alignof_spelling);
      else
	pedwarn_c99 (loc, OPT_Wpedantic, "ISO C90 does not support %qE",
		     alignof_spelling);
    }
  c_parser_consume_token (parser);
  c_inhibit_evaluation_warnings++;
  in_alignof++;
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
      && c_token_starts_typename (c_parser_peek_2nd_token (parser)))
    {
      /* Either __alignof__ ( type-name ) or __alignof__
	 unary-expression starting with a compound literal.  */
      location_t loc;
      struct c_type_name *type_name;
      struct c_expr ret;
      c_parser_consume_token (parser);
      loc = c_parser_peek_token (parser)->location;
      type_name = c_parser_type_name (parser);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
      if (type_name == NULL)
	{
	  struct c_expr ret;
	  c_inhibit_evaluation_warnings--;
	  in_alignof--;
	  ret.value = error_mark_node;
	  ret.original_code = ERROR_MARK;
	  ret.original_type = NULL;
	  return ret;
	}
      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	{
	  expr = c_parser_postfix_expression_after_paren_type (parser,
							       type_name,
							       loc);
	  goto alignof_expr;
	}
      /* alignof ( type-name ).  */
      c_inhibit_evaluation_warnings--;
      in_alignof--;
      ret.value = c_sizeof_or_alignof_type (loc, groktypename (type_name,
							       NULL, NULL),
					    false, is_c11_alignof, 1);
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      return ret;
    }
  else
    {
      struct c_expr ret;
      expr = c_parser_unary_expression (parser);
    alignof_expr:
      mark_exp_read (expr.value);
      c_inhibit_evaluation_warnings--;
      in_alignof--;
      pedwarn (loc, OPT_Wpedantic, "ISO C does not allow %<%E (expression)%>",
	       alignof_spelling);
      ret.value = c_alignof_expr (loc, expr.value);
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      return ret;
    }
}

/* Helper function to read arguments of builtins which are interfaces
   for the middle-end nodes like COMPLEX_EXPR, VEC_PERM_EXPR and
   others.  The name of the builtin is passed using BNAME parameter.
   Function returns true if there were no errors while parsing and
   stores the arguments in CEXPR_LIST.  */
static bool
c_parser_get_builtin_args (c_parser *parser, const char *bname,
			   vec<c_expr_t, va_gc> **ret_cexpr_list,
			   bool choose_expr_p)
{
  location_t loc = c_parser_peek_token (parser)->location;
  vec<c_expr_t, va_gc> *cexpr_list;
  c_expr_t expr;
  bool saved_force_folding_builtin_constant_p;

  *ret_cexpr_list = NULL;
  if (c_parser_next_token_is_not (parser, CPP_OPEN_PAREN))
    {
      error_at (loc, "cannot take address of %qs", bname);
      return false;
    }

  c_parser_consume_token (parser);

  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
    {
      c_parser_consume_token (parser);
      return true;
    }

  saved_force_folding_builtin_constant_p
    = force_folding_builtin_constant_p;
  force_folding_builtin_constant_p |= choose_expr_p;
  expr = c_parser_expr_no_commas (parser, NULL);
  force_folding_builtin_constant_p
    = saved_force_folding_builtin_constant_p;
  vec_alloc (cexpr_list, 1);
  vec_safe_push (cexpr_list, expr);
  while (c_parser_next_token_is (parser, CPP_COMMA))
    {
      c_parser_consume_token (parser);
      expr = c_parser_expr_no_commas (parser, NULL);
      vec_safe_push (cexpr_list, expr);
    }

  if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
    return false;

  *ret_cexpr_list = cexpr_list;
  return true;
}

/* This represents a single generic-association.  */

struct c_generic_association
{
  /* The location of the starting token of the type.  */
  location_t type_location;
  /* The association's type, or NULL_TREE for 'default'.  */
  tree type;
  /* The association's expression.  */
  struct c_expr expression;
};

/* Parse a generic-selection.  (C11 6.5.1.1).
   
   generic-selection:
     _Generic ( assignment-expression , generic-assoc-list )
     
   generic-assoc-list:
     generic-association
     generic-assoc-list , generic-association
   
   generic-association:
     type-name : assignment-expression
     default : assignment-expression
*/

static struct c_expr
c_parser_generic_selection (c_parser *parser)
{
  vec<c_generic_association> associations = vNULL;
  struct c_expr selector, error_expr;
  tree selector_type;
  struct c_generic_association matched_assoc;
  bool match_found = false;
  location_t generic_loc, selector_loc;

  error_expr.original_code = ERROR_MARK;
  error_expr.original_type = NULL;
  error_expr.value = error_mark_node;
  matched_assoc.type_location = UNKNOWN_LOCATION;
  matched_assoc.type = NULL_TREE;
  matched_assoc.expression = error_expr;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_GENERIC));
  generic_loc = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);
  if (flag_isoc99)
    pedwarn_c99 (generic_loc, OPT_Wpedantic,
		 "ISO C99 does not support %<_Generic%>");
  else
    pedwarn_c99 (generic_loc, OPT_Wpedantic,
		 "ISO C90 does not support %<_Generic%>");

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return error_expr;

  c_inhibit_evaluation_warnings++;
  selector_loc = c_parser_peek_token (parser)->location;
  selector = c_parser_expr_no_commas (parser, NULL);
  selector = default_function_array_conversion (selector_loc, selector);
  c_inhibit_evaluation_warnings--;

  if (selector.value == error_mark_node)
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return selector;
    }
  selector_type = TREE_TYPE (selector.value);
  /* In ISO C terms, rvalues (including the controlling expression of
     _Generic) do not have qualified types.  */
  if (TREE_CODE (selector_type) != ARRAY_TYPE)
    selector_type = TYPE_MAIN_VARIANT (selector_type);
  /* In ISO C terms, _Noreturn is not part of the type of expressions
     such as &abort, but in GCC it is represented internally as a type
     qualifier.  */
  if (FUNCTION_POINTER_TYPE_P (selector_type)
      && TYPE_QUALS (TREE_TYPE (selector_type)) != TYPE_UNQUALIFIED)
    selector_type
      = build_pointer_type (TYPE_MAIN_VARIANT (TREE_TYPE (selector_type)));

  if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return error_expr;
    }

  while (1)
    {
      struct c_generic_association assoc, *iter;
      unsigned int ix;
      c_token *token = c_parser_peek_token (parser);

      assoc.type_location = token->location;
      if (token->type == CPP_KEYWORD && token->keyword == RID_DEFAULT)
	{
	  c_parser_consume_token (parser);
	  assoc.type = NULL_TREE;
	}
      else
	{
	  struct c_type_name *type_name;

	  type_name = c_parser_type_name (parser);
	  if (type_name == NULL)
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      goto error_exit;
	    }
	  assoc.type = groktypename (type_name, NULL, NULL);
	  if (assoc.type == error_mark_node)
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      goto error_exit;
	    }

	  if (TREE_CODE (assoc.type) == FUNCTION_TYPE)
	    error_at (assoc.type_location,
		      "%<_Generic%> association has function type");
	  else if (!COMPLETE_TYPE_P (assoc.type))
	    error_at (assoc.type_location,
		      "%<_Generic%> association has incomplete type");

	  if (variably_modified_type_p (assoc.type, NULL_TREE))
	    error_at (assoc.type_location,
		      "%<_Generic%> association has "
		      "variable length type");
	}

      if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  goto error_exit;
	}

      assoc.expression = c_parser_expr_no_commas (parser, NULL);
      if (assoc.expression.value == error_mark_node)
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  goto error_exit;
	}

      for (ix = 0; associations.iterate (ix, &iter); ++ix)
	{
	  if (assoc.type == NULL_TREE)
	    {
	      if (iter->type == NULL_TREE)
		{
		  error_at (assoc.type_location,
			    "duplicate %<default%> case in %<_Generic%>");
		  inform (iter->type_location, "original %<default%> is here");
		}
	    }
	  else if (iter->type != NULL_TREE)
	    {
	      if (comptypes (assoc.type, iter->type))
		{
		  error_at (assoc.type_location,
			    "%<_Generic%> specifies two compatible types");
		  inform (iter->type_location, "compatible type is here");
		}
	    }
	}

      if (assoc.type == NULL_TREE)
	{
	  if (!match_found)
	    {
	      matched_assoc = assoc;
	      match_found = true;
	    }
	}
      else if (comptypes (assoc.type, selector_type))
	{
	  if (!match_found || matched_assoc.type == NULL_TREE)
	    {
	      matched_assoc = assoc;
	      match_found = true;
	    }
	  else
	    {
	      error_at (assoc.type_location,
			"%<_Generic> selector matches multiple associations");
	      inform (matched_assoc.type_location,
		      "other match is here");
	    }
	}

      associations.safe_push (assoc);

      if (c_parser_peek_token (parser)->type != CPP_COMMA)
	break;
      c_parser_consume_token (parser);
    }

  associations.release ();

  if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return error_expr;
    }

  if (!match_found)
    {
      error_at (selector_loc, "%<_Generic%> selector of type %qT is not "
		"compatible with any association",
		selector_type);
      return error_expr;
    }

  return matched_assoc.expression;

 error_exit:
  associations.release ();
  return error_expr;
}

/* Parse a postfix expression (C90 6.3.1-6.3.2, C99 6.5.1-6.5.2).

   postfix-expression:
     primary-expression
     postfix-expression [ expression ]
     postfix-expression ( argument-expression-list[opt] )
     postfix-expression . identifier
     postfix-expression -> identifier
     postfix-expression ++
     postfix-expression --
     ( type-name ) { initializer-list }
     ( type-name ) { initializer-list , }

   argument-expression-list:
     argument-expression
     argument-expression-list , argument-expression

   primary-expression:
     identifier
     constant
     string-literal
     ( expression )
     generic-selection

   GNU extensions:

   primary-expression:
     __func__
       (treated as a keyword in GNU C)
     __FUNCTION__
     __PRETTY_FUNCTION__
     ( compound-statement )
     __builtin_va_arg ( assignment-expression , type-name )
     __builtin_offsetof ( type-name , offsetof-member-designator )
     __builtin_choose_expr ( assignment-expression ,
			     assignment-expression ,
			     assignment-expression )
     __builtin_types_compatible_p ( type-name , type-name )
     __builtin_complex ( assignment-expression , assignment-expression )
     __builtin_shuffle ( assignment-expression , assignment-expression )
     __builtin_shuffle ( assignment-expression , 
			 assignment-expression ,
			 assignment-expression, )

   offsetof-member-designator:
     identifier
     offsetof-member-designator . identifier
     offsetof-member-designator [ expression ]

   Objective-C:

   primary-expression:
     [ objc-receiver objc-message-args ]
     @selector ( objc-selector-arg )
     @protocol ( identifier )
     @encode ( type-name )
     objc-string-literal
     Classname . identifier
*/

static struct c_expr
c_parser_postfix_expression (c_parser *parser)
{
  struct c_expr expr, e1;
  struct c_type_name *t1, *t2;
  location_t loc = c_parser_peek_token (parser)->location;;
  expr.original_code = ERROR_MARK;
  expr.original_type = NULL;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_NUMBER:
      expr.value = c_parser_peek_token (parser)->value;
      loc = c_parser_peek_token (parser)->location;
      c_parser_consume_token (parser);
      if (TREE_CODE (expr.value) == FIXED_CST
	  && !targetm.fixed_point_supported_p ())
	{
	  error_at (loc, "fixed-point types not supported for this target");
	  expr.value = error_mark_node;
	}
      break;
    case CPP_CHAR:
    case CPP_CHAR16:
    case CPP_CHAR32:
    case CPP_WCHAR:
      expr.value = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
      break;
    case CPP_STRING:
    case CPP_STRING16:
    case CPP_STRING32:
    case CPP_WSTRING:
    case CPP_UTF8STRING:
      expr.value = c_parser_peek_token (parser)->value;
      expr.original_code = STRING_CST;
      c_parser_consume_token (parser);
      break;
    case CPP_OBJC_STRING:
      gcc_assert (c_dialect_objc ());
      expr.value
	= objc_build_string_object (c_parser_peek_token (parser)->value);
      c_parser_consume_token (parser);
      break;
    case CPP_NAME:
      switch (c_parser_peek_token (parser)->id_kind)
	{
	case C_ID_ID:
	  {
	    tree id = c_parser_peek_token (parser)->value;
	    c_parser_consume_token (parser);
	    expr.value = build_external_ref (loc, id,
					     (c_parser_peek_token (parser)->type
					      == CPP_OPEN_PAREN),
					     &expr.original_type);
	    break;
	  }
	case C_ID_CLASSNAME:
	  {
	    /* Here we parse the Objective-C 2.0 Class.name dot
	       syntax.  */
	    tree class_name = c_parser_peek_token (parser)->value;
	    tree component;
	    c_parser_consume_token (parser);
	    gcc_assert (c_dialect_objc ());
	    if (!c_parser_require (parser, CPP_DOT, "expected %<.%>"))
	      {
		expr.value = error_mark_node;
		break;
	      }
	    if (c_parser_next_token_is_not (parser, CPP_NAME))
	      {
		c_parser_error (parser, "expected identifier");
		expr.value = error_mark_node;
		break;
	      }
	    component = c_parser_peek_token (parser)->value;
	    c_parser_consume_token (parser);
	    expr.value = objc_build_class_component_ref (class_name, 
							 component);
	    break;
	  }
	default:
	  c_parser_error (parser, "expected expression");
	  expr.value = error_mark_node;
	  break;
	}
      break;
    case CPP_OPEN_PAREN:
      /* A parenthesized expression, statement expression or compound
	 literal.  */
      if (c_parser_peek_2nd_token (parser)->type == CPP_OPEN_BRACE)
	{
	  /* A statement expression.  */
	  tree stmt;
	  location_t brace_loc;
	  c_parser_consume_token (parser);
	  brace_loc = c_parser_peek_token (parser)->location;
	  c_parser_consume_token (parser);
	  if (!building_stmt_list_p ())
	    {
	      error_at (loc, "braced-group within expression allowed "
			"only inside a function");
	      parser->error = true;
	      c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      expr.value = error_mark_node;
	      break;
	    }
	  stmt = c_begin_stmt_expr ();
	  c_parser_compound_statement_nostart (parser);
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  pedwarn (loc, OPT_Wpedantic,
		   "ISO C forbids braced-groups within expressions");
	  expr.value = c_finish_stmt_expr (brace_loc, stmt);
	  mark_exp_read (expr.value);
	}
      else if (c_token_starts_typename (c_parser_peek_2nd_token (parser)))
	{
	  /* A compound literal.  ??? Can we actually get here rather
	     than going directly to
	     c_parser_postfix_expression_after_paren_type from
	     elsewhere?  */
	  location_t loc;
	  struct c_type_name *type_name;
	  c_parser_consume_token (parser);
	  loc = c_parser_peek_token (parser)->location;
	  type_name = c_parser_type_name (parser);
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  if (type_name == NULL)
	    {
	      expr.value = error_mark_node;
	    }
	  else
	    expr = c_parser_postfix_expression_after_paren_type (parser,
								 type_name,
								 loc);
	}
      else
	{
	  /* A parenthesized expression.  */
	  c_parser_consume_token (parser);
	  expr = c_parser_expression (parser);
	  if (TREE_CODE (expr.value) == MODIFY_EXPR)
	    TREE_NO_WARNING (expr.value) = 1;
	  if (expr.original_code != C_MAYBE_CONST_EXPR)
	    expr.original_code = ERROR_MARK;
	  /* Don't change EXPR.ORIGINAL_TYPE.  */
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	}
      break;
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_FUNCTION_NAME:
	  pedwarn (loc, OPT_Wpedantic,  "ISO C does not support "
		   "%<__FUNCTION__%> predefined identifier");
	  expr.value = fname_decl (loc,
				   c_parser_peek_token (parser)->keyword,
				   c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	case RID_PRETTY_FUNCTION_NAME:
	  pedwarn (loc, OPT_Wpedantic,  "ISO C does not support "
		   "%<__PRETTY_FUNCTION__%> predefined identifier");
	  expr.value = fname_decl (loc,
				   c_parser_peek_token (parser)->keyword,
				   c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	case RID_C99_FUNCTION_NAME:
	  pedwarn_c90 (loc, OPT_Wpedantic,  "ISO C90 does not support "
		   "%<__func__%> predefined identifier");
	  expr.value = fname_decl (loc,
				   c_parser_peek_token (parser)->keyword,
				   c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	case RID_VA_ARG:
	  c_parser_consume_token (parser);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  e1 = c_parser_expr_no_commas (parser, NULL);
	  mark_exp_read (e1.value);
	  e1.value = c_fully_fold (e1.value, false, NULL);
	  if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      expr.value = error_mark_node;
	      break;
	    }
	  loc = c_parser_peek_token (parser)->location;
	  t1 = c_parser_type_name (parser);
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  if (t1 == NULL)
	    {
	      expr.value = error_mark_node;
	    }
	  else
	    {
	      tree type_expr = NULL_TREE;
	      expr.value = c_build_va_arg (loc, e1.value,
					   groktypename (t1, &type_expr, NULL));
	      if (type_expr)
		{
		  expr.value = build2 (C_MAYBE_CONST_EXPR,
				       TREE_TYPE (expr.value), type_expr,
				       expr.value);
		  C_MAYBE_CONST_EXPR_NON_CONST (expr.value) = true;
		}
	    }
	  break;
	case RID_OFFSETOF:
	  c_parser_consume_token (parser);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  t1 = c_parser_type_name (parser);
	  if (t1 == NULL)
	    parser->error = true;
	  if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
            gcc_assert (parser->error);
	  if (parser->error)
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      expr.value = error_mark_node;
	      break;
	    }

	  {
	    tree type = groktypename (t1, NULL, NULL);
	    tree offsetof_ref;
	    if (type == error_mark_node)
	      offsetof_ref = error_mark_node;
	    else
	      {
		offsetof_ref = build1 (INDIRECT_REF, type, null_pointer_node);
		SET_EXPR_LOCATION (offsetof_ref, loc);
	      }
	    /* Parse the second argument to __builtin_offsetof.  We
	       must have one identifier, and beyond that we want to
	       accept sub structure and sub array references.  */
	    if (c_parser_next_token_is (parser, CPP_NAME))
	      {
		offsetof_ref = build_component_ref
		  (loc, offsetof_ref, c_parser_peek_token (parser)->value);
		c_parser_consume_token (parser);
		while (c_parser_next_token_is (parser, CPP_DOT)
		       || c_parser_next_token_is (parser,
						  CPP_OPEN_SQUARE)
		       || c_parser_next_token_is (parser,
						  CPP_DEREF))
		  {
		    if (c_parser_next_token_is (parser, CPP_DEREF))
		      {
			loc = c_parser_peek_token (parser)->location;
			offsetof_ref = build_array_ref (loc,
							offsetof_ref,
							integer_zero_node);
			goto do_dot;
		      }
		    else if (c_parser_next_token_is (parser, CPP_DOT))
		      {
		      do_dot:
			c_parser_consume_token (parser);
			if (c_parser_next_token_is_not (parser,
							CPP_NAME))
			  {
			    c_parser_error (parser, "expected identifier");
			    break;
			  }
			offsetof_ref = build_component_ref
			  (loc, offsetof_ref,
			   c_parser_peek_token (parser)->value);
			c_parser_consume_token (parser);
		      }
		    else
		      {
			struct c_expr ce;
			tree idx;
			loc = c_parser_peek_token (parser)->location;
			c_parser_consume_token (parser);
			ce = c_parser_expression (parser);
			ce = convert_lvalue_to_rvalue (loc, ce, false, false);
			idx = ce.value;
			idx = c_fully_fold (idx, false, NULL);
			c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
						   "expected %<]%>");
			offsetof_ref = build_array_ref (loc, offsetof_ref, idx);
		      }
		  }
	      }
	    else
	      c_parser_error (parser, "expected identifier");
	    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				       "expected %<)%>");
	    expr.value = fold_offsetof (offsetof_ref);
	  }
	  break;
	case RID_CHOOSE_EXPR:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    c_expr_t *e1_p, *e2_p, *e3_p;
	    tree c;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_choose_expr",
					    &cexpr_list, true))
	      {
		expr.value = error_mark_node;
		break;
	      }

	    if (vec_safe_length (cexpr_list) != 3)
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_choose_expr%>");
		expr.value = error_mark_node;
		break;
	      }

	    e1_p = &(*cexpr_list)[0];
	    e2_p = &(*cexpr_list)[1];
	    e3_p = &(*cexpr_list)[2];

	    c = e1_p->value;
	    mark_exp_read (e2_p->value);
	    mark_exp_read (e3_p->value);
	    if (TREE_CODE (c) != INTEGER_CST
		|| !INTEGRAL_TYPE_P (TREE_TYPE (c)))
	      error_at (loc,
			"first argument to %<__builtin_choose_expr%> not"
			" a constant");
	    constant_expression_warning (c);
	    expr = integer_zerop (c) ? *e3_p : *e2_p;
	    break;
	  }
	case RID_TYPES_COMPATIBLE_P:
	  c_parser_consume_token (parser);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  t1 = c_parser_type_name (parser);
	  if (t1 == NULL)
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      expr.value = error_mark_node;
	      break;
	    }
	  t2 = c_parser_type_name (parser);
	  if (t2 == NULL)
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  {
	    tree e1, e2;
	    e1 = groktypename (t1, NULL, NULL);
	    e2 = groktypename (t2, NULL, NULL);
	    if (e1 == error_mark_node || e2 == error_mark_node)
	      {
		expr.value = error_mark_node;
		break;
	      }

	    e1 = TYPE_MAIN_VARIANT (e1);
	    e2 = TYPE_MAIN_VARIANT (e2);

	    expr.value
	      = comptypes (e1, e2) ? integer_one_node : integer_zero_node;
	  }
	  break;
	case RID_BUILTIN_CALL_WITH_STATIC_CHAIN:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    c_expr_t *e2_p;
	    tree chain_value;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_call_with_static_chain",
					    &cexpr_list, false))
	      {
		expr.value = error_mark_node;
		break;
	      }
	    if (vec_safe_length (cexpr_list) != 2)
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_call_with_static_chain%>");
		expr.value = error_mark_node;
		break;
	      }

	    expr = (*cexpr_list)[0];
	    e2_p = &(*cexpr_list)[1];
	    *e2_p = convert_lvalue_to_rvalue (loc, *e2_p, true, true);
	    chain_value = e2_p->value;
	    mark_exp_read (chain_value);

	    if (TREE_CODE (expr.value) != CALL_EXPR)
	      error_at (loc, "first argument to "
			"%<__builtin_call_with_static_chain%> "
			"must be a call expression");
	    else if (TREE_CODE (TREE_TYPE (chain_value)) != POINTER_TYPE)
	      error_at (loc, "second argument to "
			"%<__builtin_call_with_static_chain%> "
			"must be a pointer type");
	    else
	      CALL_EXPR_STATIC_CHAIN (expr.value) = chain_value;
	    break;
	  }
	case RID_BUILTIN_COMPLEX:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    c_expr_t *e1_p, *e2_p;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_complex",
					    &cexpr_list, false))
	      {
		expr.value = error_mark_node;
		break;
	      }

	    if (vec_safe_length (cexpr_list) != 2)
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_complex%>");
		expr.value = error_mark_node;
		break;
	      }

	    e1_p = &(*cexpr_list)[0];
	    e2_p = &(*cexpr_list)[1];

	    *e1_p = convert_lvalue_to_rvalue (loc, *e1_p, true, true);
	    if (TREE_CODE (e1_p->value) == EXCESS_PRECISION_EXPR)
	      e1_p->value = convert (TREE_TYPE (e1_p->value),
				     TREE_OPERAND (e1_p->value, 0));
	    *e2_p = convert_lvalue_to_rvalue (loc, *e2_p, true, true);
	    if (TREE_CODE (e2_p->value) == EXCESS_PRECISION_EXPR)
	      e2_p->value = convert (TREE_TYPE (e2_p->value),
				     TREE_OPERAND (e2_p->value, 0));
	    if (!SCALAR_FLOAT_TYPE_P (TREE_TYPE (e1_p->value))
		|| DECIMAL_FLOAT_TYPE_P (TREE_TYPE (e1_p->value))
		|| !SCALAR_FLOAT_TYPE_P (TREE_TYPE (e2_p->value))
		|| DECIMAL_FLOAT_TYPE_P (TREE_TYPE (e2_p->value)))
	      {
		error_at (loc, "%<__builtin_complex%> operand "
			  "not of real binary floating-point type");
		expr.value = error_mark_node;
		break;
	      }
	    if (TYPE_MAIN_VARIANT (TREE_TYPE (e1_p->value))
		!= TYPE_MAIN_VARIANT (TREE_TYPE (e2_p->value)))
	      {
		error_at (loc,
			  "%<__builtin_complex%> operands of different types");
		expr.value = error_mark_node;
		break;
	      }
	    pedwarn_c90 (loc, OPT_Wpedantic,
			 "ISO C90 does not support complex types");
	    expr.value = build2 (COMPLEX_EXPR,
				 build_complex_type
				   (TYPE_MAIN_VARIANT
				     (TREE_TYPE (e1_p->value))),
				 e1_p->value, e2_p->value);
	    break;
	  }
	case RID_BUILTIN_SHUFFLE:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    unsigned int i;
	    c_expr_t *p;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_shuffle",
					    &cexpr_list, false))
	      {
		expr.value = error_mark_node;
		break;
	      }

	    FOR_EACH_VEC_SAFE_ELT (cexpr_list, i, p)
	      *p = convert_lvalue_to_rvalue (loc, *p, true, true);

	    if (vec_safe_length (cexpr_list) == 2)
	      expr.value =
		c_build_vec_perm_expr
		  (loc, (*cexpr_list)[0].value,
		   NULL_TREE, (*cexpr_list)[1].value);

	    else if (vec_safe_length (cexpr_list) == 3)
	      expr.value =
		c_build_vec_perm_expr
		  (loc, (*cexpr_list)[0].value,
		   (*cexpr_list)[1].value,
		   (*cexpr_list)[2].value);
	    else
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_shuffle%>");
		expr.value = error_mark_node;
	      }
	    break;
	  }
	case RID_AT_SELECTOR:
	  gcc_assert (c_dialect_objc ());
	  c_parser_consume_token (parser);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  {
	    tree sel = c_parser_objc_selector_arg (parser);
	    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				       "expected %<)%>");
	    expr.value = objc_build_selector_expr (loc, sel);
	  }
	  break;
	case RID_AT_PROTOCOL:
	  gcc_assert (c_dialect_objc ());
	  c_parser_consume_token (parser);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  if (c_parser_next_token_is_not (parser, CPP_NAME))
	    {
	      c_parser_error (parser, "expected identifier");
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      expr.value = error_mark_node;
	      break;
	    }
	  {
	    tree id = c_parser_peek_token (parser)->value;
	    c_parser_consume_token (parser);
	    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				       "expected %<)%>");
	    expr.value = objc_build_protocol_expr (id);
	  }
	  break;
	case RID_AT_ENCODE:
	  /* Extension to support C-structures in the archiver.  */
	  gcc_assert (c_dialect_objc ());
	  c_parser_consume_token (parser);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    {
	      expr.value = error_mark_node;
	      break;
	    }
	  t1 = c_parser_type_name (parser);
	  if (t1 == NULL)
	    {
	      expr.value = error_mark_node;
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      break;
	    }
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  {
	    tree type = groktypename (t1, NULL, NULL);
	    expr.value = objc_build_encode_expr (type);
	  }
	  break;
	case RID_GENERIC:
	  expr = c_parser_generic_selection (parser);
	  break;
	case RID_CILK_SPAWN:
	  c_parser_consume_token (parser);
	  if (!flag_cilkplus)
	    {
	      error_at (loc, "-fcilkplus must be enabled to use "
			"%<_Cilk_spawn%>");
	      expr = c_parser_postfix_expression (parser);
	      expr.value = error_mark_node;	      
	    }
	  else if (c_parser_peek_token (parser)->keyword == RID_CILK_SPAWN)
	    {
	      error_at (loc, "consecutive %<_Cilk_spawn%> keywords "
			"are not permitted");
	      /* Now flush out all the _Cilk_spawns.  */
	      while (c_parser_peek_token (parser)->keyword == RID_CILK_SPAWN)
		c_parser_consume_token (parser);
	      expr = c_parser_postfix_expression (parser);
	    }
	  else
	    {
	      expr = c_parser_postfix_expression (parser);
	      expr.value = build_cilk_spawn (loc, expr.value);
	    }
	  break; 
	default:
	  c_parser_error (parser, "expected expression");
	  expr.value = error_mark_node;
	  break;
	}
      break;
    case CPP_OPEN_SQUARE:
      if (c_dialect_objc ())
	{
	  tree receiver, args;
	  c_parser_consume_token (parser);
	  receiver = c_parser_objc_receiver (parser);
	  args = c_parser_objc_message_args (parser);
	  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
				     "expected %<]%>");
	  expr.value = objc_build_message_expr (receiver, args);
	  break;
	}
      /* Else fall through to report error.  */
    default:
      c_parser_error (parser, "expected expression");
      expr.value = error_mark_node;
      break;
    }
  return c_parser_postfix_expression_after_primary (parser, loc, expr);
}

/* Parse a postfix expression after a parenthesized type name: the
   brace-enclosed initializer of a compound literal, possibly followed
   by some postfix operators.  This is separate because it is not
   possible to tell until after the type name whether a cast
   expression has a cast or a compound literal, or whether the operand
   of sizeof is a parenthesized type name or starts with a compound
   literal.  TYPE_LOC is the location where TYPE_NAME starts--the
   location of the first token after the parentheses around the type
   name.  */

static struct c_expr
c_parser_postfix_expression_after_paren_type (c_parser *parser,
					      struct c_type_name *type_name,
					      location_t type_loc)
{
  tree type;
  struct c_expr init;
  bool non_const;
  struct c_expr expr;
  location_t start_loc;
  tree type_expr = NULL_TREE;
  bool type_expr_const = true;
  check_compound_literal_type (type_loc, type_name);
  start_init (NULL_TREE, NULL, 0);
  type = groktypename (type_name, &type_expr, &type_expr_const);
  start_loc = c_parser_peek_token (parser)->location;
  if (type != error_mark_node && C_TYPE_VARIABLE_SIZE (type))
    {
      error_at (type_loc, "compound literal has variable size");
      type = error_mark_node;
    }
  init = c_parser_braced_init (parser, type, false);
  finish_init ();
  maybe_warn_string_init (type_loc, type, init);

  if (type != error_mark_node
      && !ADDR_SPACE_GENERIC_P (TYPE_ADDR_SPACE (type))
      && current_function_decl)
    {
      error ("compound literal qualified by address-space qualifier");
      type = error_mark_node;
    }

  pedwarn_c90 (start_loc, OPT_Wpedantic, "ISO C90 forbids compound literals");
  non_const = ((init.value && TREE_CODE (init.value) == CONSTRUCTOR)
	       ? CONSTRUCTOR_NON_CONST (init.value)
	       : init.original_code == C_MAYBE_CONST_EXPR);
  non_const |= !type_expr_const;
  expr.value = build_compound_literal (start_loc, type, init.value, non_const);
  expr.original_code = ERROR_MARK;
  expr.original_type = NULL;
  if (type_expr)
    {
      if (TREE_CODE (expr.value) == C_MAYBE_CONST_EXPR)
	{
	  gcc_assert (C_MAYBE_CONST_EXPR_PRE (expr.value) == NULL_TREE);
	  C_MAYBE_CONST_EXPR_PRE (expr.value) = type_expr;
	}
      else
	{
	  gcc_assert (!non_const);
	  expr.value = build2 (C_MAYBE_CONST_EXPR, type,
			       type_expr, expr.value);
	}
    }
  return c_parser_postfix_expression_after_primary (parser, start_loc, expr);
}

/* Callback function for sizeof_pointer_memaccess_warning to compare
   types.  */

static bool
sizeof_ptr_memacc_comptypes (tree type1, tree type2)
{
  return comptypes (type1, type2) == 1;
}

/* Parse a postfix expression after the initial primary or compound
   literal; that is, parse a series of postfix operators.

   EXPR_LOC is the location of the primary expression.  */

static struct c_expr
c_parser_postfix_expression_after_primary (c_parser *parser,
					   location_t expr_loc,
					   struct c_expr expr)
{
  struct c_expr orig_expr;
  tree ident, idx;
  location_t sizeof_arg_loc[3];
  tree sizeof_arg[3];
  unsigned int literal_zero_mask;
  unsigned int i;
  vec<tree, va_gc> *exprlist;
  vec<tree, va_gc> *origtypes = NULL;
  vec<location_t> arg_loc = vNULL;

  while (true)
    {
      location_t op_loc = c_parser_peek_token (parser)->location;
      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_OPEN_SQUARE:
	  /* Array reference.  */
	  c_parser_consume_token (parser);
	  if (flag_cilkplus
	      && c_parser_peek_token (parser)->type == CPP_COLON)
	    /* If we are here, then we have something like this:
	       Array [ : ]
	    */
	    expr.value = c_parser_array_notation (expr_loc, parser, NULL_TREE,
						  expr.value);
	  else
	    {	      
	      idx = c_parser_expression (parser).value;
	      /* Here we have 3 options:
		 1. Array [EXPR] -- Normal Array call.
		 2. Array [EXPR : EXPR] -- Array notation without stride.
		 3. Array [EXPR : EXPR : EXPR] -- Array notation with stride.

		 For 1, we just handle it just like a normal array expression.
		 For 2 and 3 we handle it like we handle array notations.  The
		 idx value we have above becomes the initial/start index.
	      */
	      if (flag_cilkplus
		  && c_parser_peek_token (parser)->type == CPP_COLON)
		expr.value = c_parser_array_notation (expr_loc, parser, idx, 
						      expr.value);
	      else
		{
		  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
					     "expected %<]%>");
		  expr.value = build_array_ref (op_loc, expr.value, idx);
		}
	    }
	  expr.original_code = ERROR_MARK;
	  expr.original_type = NULL;
	  break;
	case CPP_OPEN_PAREN:
	  /* Function call.  */
	  c_parser_consume_token (parser);
	  for (i = 0; i < 3; i++)
	    {
	      sizeof_arg[i] = NULL_TREE;
	      sizeof_arg_loc[i] = UNKNOWN_LOCATION;
	    }
	  literal_zero_mask = 0;
	  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    exprlist = NULL;
	  else
	    exprlist = c_parser_expr_list (parser, true, false, &origtypes,
					   sizeof_arg_loc, sizeof_arg,
					   &arg_loc, &literal_zero_mask);
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  orig_expr = expr;
	  mark_exp_read (expr.value);
	  if (warn_sizeof_pointer_memaccess)
	    sizeof_pointer_memaccess_warning (sizeof_arg_loc,
					      expr.value, exprlist,
					      sizeof_arg,
					      sizeof_ptr_memacc_comptypes);
	  if (warn_memset_transposed_args
	      && TREE_CODE (expr.value) == FUNCTION_DECL
	      && DECL_BUILT_IN_CLASS (expr.value) == BUILT_IN_NORMAL
	      && DECL_FUNCTION_CODE (expr.value) == BUILT_IN_MEMSET
	      && vec_safe_length (exprlist) == 3
	      && integer_zerop ((*exprlist)[2])
	      && (literal_zero_mask & (1 << 2)) != 0
	      && (!integer_zerop ((*exprlist)[1])
		  || (literal_zero_mask & (1 << 1)) == 0))
	    warning_at (expr_loc, OPT_Wmemset_transposed_args,
			"%<memset%> used with constant zero length parameter; "
			"this could be due to transposed parameters");

	  expr.value
	    = c_build_function_call_vec (expr_loc, arg_loc, expr.value,
					 exprlist, origtypes);
	  expr.original_code = ERROR_MARK;
	  if (TREE_CODE (expr.value) == INTEGER_CST
	      && TREE_CODE (orig_expr.value) == FUNCTION_DECL
	      && DECL_BUILT_IN_CLASS (orig_expr.value) == BUILT_IN_NORMAL
	      && DECL_FUNCTION_CODE (orig_expr.value) == BUILT_IN_CONSTANT_P)
	    expr.original_code = C_MAYBE_CONST_EXPR;
	  expr.original_type = NULL;
	  if (exprlist)
	    {
	      release_tree_vector (exprlist);
	      release_tree_vector (origtypes);
	    }
	  arg_loc.release ();
	  break;
	case CPP_DOT:
	  /* Structure element reference.  */
	  c_parser_consume_token (parser);
	  expr = default_function_array_conversion (expr_loc, expr);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    ident = c_parser_peek_token (parser)->value;
	  else
	    {
	      c_parser_error (parser, "expected identifier");
	      expr.value = error_mark_node;
	      expr.original_code = ERROR_MARK;
              expr.original_type = NULL;
	      return expr;
	    }
	  c_parser_consume_token (parser);
	  expr.value = build_component_ref (op_loc, expr.value, ident);
	  expr.original_code = ERROR_MARK;
	  if (TREE_CODE (expr.value) != COMPONENT_REF)
	    expr.original_type = NULL;
	  else
	    {
	      /* Remember the original type of a bitfield.  */
	      tree field = TREE_OPERAND (expr.value, 1);
	      if (TREE_CODE (field) != FIELD_DECL)
		expr.original_type = NULL;
	      else
		expr.original_type = DECL_BIT_FIELD_TYPE (field);
	    }
	  break;
	case CPP_DEREF:
	  /* Structure element reference.  */
	  c_parser_consume_token (parser);
	  expr = convert_lvalue_to_rvalue (expr_loc, expr, true, false);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    ident = c_parser_peek_token (parser)->value;
	  else
	    {
	      c_parser_error (parser, "expected identifier");
	      expr.value = error_mark_node;
	      expr.original_code = ERROR_MARK;
	      expr.original_type = NULL;
	      return expr;
	    }
	  c_parser_consume_token (parser);
	  expr.value = build_component_ref (op_loc,
					    build_indirect_ref (op_loc,
								expr.value,
								RO_ARROW),
					    ident);
	  expr.original_code = ERROR_MARK;
	  if (TREE_CODE (expr.value) != COMPONENT_REF)
	    expr.original_type = NULL;
	  else
	    {
	      /* Remember the original type of a bitfield.  */
	      tree field = TREE_OPERAND (expr.value, 1);
	      if (TREE_CODE (field) != FIELD_DECL)
		expr.original_type = NULL;
	      else
		expr.original_type = DECL_BIT_FIELD_TYPE (field);
	    }
	  break;
	case CPP_PLUS_PLUS:
	  /* Postincrement.  */
	  c_parser_consume_token (parser);
	  /* If the expressions have array notations, we expand them.  */
	  if (flag_cilkplus
	      && TREE_CODE (expr.value) == ARRAY_NOTATION_REF)
	    expr = fix_array_notation_expr (expr_loc, POSTINCREMENT_EXPR, expr);
	  else
	    {
	      expr = default_function_array_read_conversion (expr_loc, expr);
	      expr.value = build_unary_op (op_loc,
					   POSTINCREMENT_EXPR, expr.value, 0);
	    }
	  expr.original_code = ERROR_MARK;
	  expr.original_type = NULL;
	  break;
	case CPP_MINUS_MINUS:
	  /* Postdecrement.  */
	  c_parser_consume_token (parser);
	  /* If the expressions have array notations, we expand them.  */
	  if (flag_cilkplus
	      && TREE_CODE (expr.value) == ARRAY_NOTATION_REF)
	    expr = fix_array_notation_expr (expr_loc, POSTDECREMENT_EXPR, expr);
	  else
	    {
	      expr = default_function_array_read_conversion (expr_loc, expr);
	      expr.value = build_unary_op (op_loc,
					   POSTDECREMENT_EXPR, expr.value, 0);
	    }
	  expr.original_code = ERROR_MARK;
	  expr.original_type = NULL;
	  break;
	default:
	  return expr;
	}
    }
}

/* Parse an expression (C90 6.3.17, C99 6.5.17).

   expression:
     assignment-expression
     expression , assignment-expression
*/

static struct c_expr
c_parser_expression (c_parser *parser)
{
  location_t tloc = c_parser_peek_token (parser)->location;
  struct c_expr expr;
  expr = c_parser_expr_no_commas (parser, NULL);
  if (c_parser_next_token_is (parser, CPP_COMMA))
    expr = convert_lvalue_to_rvalue (tloc, expr, true, false);
  while (c_parser_next_token_is (parser, CPP_COMMA))
    {
      struct c_expr next;
      tree lhsval;
      location_t loc = c_parser_peek_token (parser)->location;
      location_t expr_loc;
      c_parser_consume_token (parser);
      expr_loc = c_parser_peek_token (parser)->location;
      lhsval = expr.value;
      while (TREE_CODE (lhsval) == COMPOUND_EXPR)
	lhsval = TREE_OPERAND (lhsval, 1);
      if (DECL_P (lhsval) || handled_component_p (lhsval))
	mark_exp_read (lhsval);
      next = c_parser_expr_no_commas (parser, NULL);
      next = convert_lvalue_to_rvalue (expr_loc, next, true, false);
      expr.value = build_compound_expr (loc, expr.value, next.value);
      expr.original_code = COMPOUND_EXPR;
      expr.original_type = next.original_type;
    }
  return expr;
}

/* Parse an expression and convert functions or arrays to pointers and
   lvalues to rvalues.  */

static struct c_expr
c_parser_expression_conv (c_parser *parser)
{
  struct c_expr expr;
  location_t loc = c_parser_peek_token (parser)->location;
  expr = c_parser_expression (parser);
  expr = convert_lvalue_to_rvalue (loc, expr, true, false);
  return expr;
}

/* Helper function of c_parser_expr_list.  Check if IDXth (0 based)
   argument is a literal zero alone and if so, set it in literal_zero_mask.  */

static inline void
c_parser_check_literal_zero (c_parser *parser, unsigned *literal_zero_mask,
			     unsigned int idx)
{
  if (idx >= HOST_BITS_PER_INT)
    return;

  c_token *tok = c_parser_peek_token (parser);
  switch (tok->type)
    {
    case CPP_NUMBER:
    case CPP_CHAR:
    case CPP_WCHAR:
    case CPP_CHAR16:
    case CPP_CHAR32:
      /* If a parameter is literal zero alone, remember it
	 for -Wmemset-transposed-args warning.  */
      if (integer_zerop (tok->value)
	  && !TREE_OVERFLOW (tok->value)
	  && (c_parser_peek_2nd_token (parser)->type == CPP_COMMA
	      || c_parser_peek_2nd_token (parser)->type == CPP_CLOSE_PAREN))
	*literal_zero_mask |= 1U << idx;
    default:
      break;
    }
}

/* Parse a non-empty list of expressions.  If CONVERT_P, convert
   functions and arrays to pointers and lvalues to rvalues.  If
   FOLD_P, fold the expressions.  If LOCATIONS is non-NULL, save the
   locations of function arguments into this vector.

   nonempty-expr-list:
     assignment-expression
     nonempty-expr-list , assignment-expression
*/

static vec<tree, va_gc> *
c_parser_expr_list (c_parser *parser, bool convert_p, bool fold_p,
		    vec<tree, va_gc> **p_orig_types,
		    location_t *sizeof_arg_loc, tree *sizeof_arg,
		    vec<location_t> *locations,
		    unsigned int *literal_zero_mask)
{
  vec<tree, va_gc> *ret;
  vec<tree, va_gc> *orig_types;
  struct c_expr expr;
  location_t loc = c_parser_peek_token (parser)->location;
  location_t cur_sizeof_arg_loc = UNKNOWN_LOCATION;
  unsigned int idx = 0;

  ret = make_tree_vector ();
  if (p_orig_types == NULL)
    orig_types = NULL;
  else
    orig_types = make_tree_vector ();

  if (sizeof_arg != NULL
      && c_parser_next_token_is_keyword (parser, RID_SIZEOF))
    cur_sizeof_arg_loc = c_parser_peek_2nd_token (parser)->location;
  if (literal_zero_mask)
    c_parser_check_literal_zero (parser, literal_zero_mask, 0);
  expr = c_parser_expr_no_commas (parser, NULL);
  if (convert_p)
    expr = convert_lvalue_to_rvalue (loc, expr, true, true);
  if (fold_p)
    expr.value = c_fully_fold (expr.value, false, NULL);
  ret->quick_push (expr.value);
  if (orig_types)
    orig_types->quick_push (expr.original_type);
  if (locations)
    locations->safe_push (loc);
  if (sizeof_arg != NULL
      && cur_sizeof_arg_loc != UNKNOWN_LOCATION
      && expr.original_code == SIZEOF_EXPR)
    {
      sizeof_arg[0] = c_last_sizeof_arg;
      sizeof_arg_loc[0] = cur_sizeof_arg_loc;
    }
  while (c_parser_next_token_is (parser, CPP_COMMA))
    {
      c_parser_consume_token (parser);
      loc = c_parser_peek_token (parser)->location;
      if (sizeof_arg != NULL
	  && c_parser_next_token_is_keyword (parser, RID_SIZEOF))
	cur_sizeof_arg_loc = c_parser_peek_2nd_token (parser)->location;
      else
	cur_sizeof_arg_loc = UNKNOWN_LOCATION;
      if (literal_zero_mask)
	c_parser_check_literal_zero (parser, literal_zero_mask, idx + 1);
      expr = c_parser_expr_no_commas (parser, NULL);
      if (convert_p)
	expr = convert_lvalue_to_rvalue (loc, expr, true, true);
      if (fold_p)
	expr.value = c_fully_fold (expr.value, false, NULL);
      vec_safe_push (ret, expr.value);
      if (orig_types)
	vec_safe_push (orig_types, expr.original_type);
      if (locations)
	locations->safe_push (loc);
      if (++idx < 3
	  && sizeof_arg != NULL
	  && cur_sizeof_arg_loc != UNKNOWN_LOCATION
	  && expr.original_code == SIZEOF_EXPR)
	{
	  sizeof_arg[idx] = c_last_sizeof_arg;
	  sizeof_arg_loc[idx] = cur_sizeof_arg_loc;
	}
    }
  if (orig_types)
    *p_orig_types = orig_types;
  return ret;
}

/* Parse Objective-C-specific constructs.  */

/* Parse an objc-class-definition.

   objc-class-definition:
     @interface identifier objc-superclass[opt] objc-protocol-refs[opt]
       objc-class-instance-variables[opt] objc-methodprotolist @end
     @implementation identifier objc-superclass[opt]
       objc-class-instance-variables[opt]
     @interface identifier ( identifier ) objc-protocol-refs[opt]
       objc-methodprotolist @end
     @interface identifier ( ) objc-protocol-refs[opt]
       objc-methodprotolist @end
     @implementation identifier ( identifier )

   objc-superclass:
     : identifier

   "@interface identifier (" must start "@interface identifier (
   identifier ) ...": objc-methodprotolist in the first production may
   not start with a parenthesized identifier as a declarator of a data
   definition with no declaration specifiers if the objc-superclass,
   objc-protocol-refs and objc-class-instance-variables are omitted.  */

static void
c_parser_objc_class_definition (c_parser *parser, tree attributes)
{
  bool iface_p;
  tree id1;
  tree superclass;
  if (c_parser_next_token_is_keyword (parser, RID_AT_INTERFACE))
    iface_p = true;
  else if (c_parser_next_token_is_keyword (parser, RID_AT_IMPLEMENTATION))
    iface_p = false;
  else
    gcc_unreachable ();

  c_parser_consume_token (parser);
  if (c_parser_next_token_is_not (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected identifier");
      return;
    }
  id1 = c_parser_peek_token (parser)->value;
  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      /* We have a category or class extension.  */
      tree id2;
      tree proto = NULL_TREE;
      c_parser_consume_token (parser);
      if (c_parser_next_token_is_not (parser, CPP_NAME))
	{
	  if (iface_p && c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    {
	      /* We have a class extension.  */
	      id2 = NULL_TREE;
	    }
	  else
	    {
	      c_parser_error (parser, "expected identifier or %<)%>");
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      return;
	    }
	}
      else
	{
	  id2 = c_parser_peek_token (parser)->value;
	  c_parser_consume_token (parser);
	}
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
      if (!iface_p)
	{
	  objc_start_category_implementation (id1, id2);
	  return;
	}
      if (c_parser_next_token_is (parser, CPP_LESS))
	proto = c_parser_objc_protocol_refs (parser);
      objc_start_category_interface (id1, id2, proto, attributes);
      c_parser_objc_methodprotolist (parser);
      c_parser_require_keyword (parser, RID_AT_END, "expected %<@end%>");
      objc_finish_interface ();
      return;
    }
  if (c_parser_next_token_is (parser, CPP_COLON))
    {
      c_parser_consume_token (parser);
      if (c_parser_next_token_is_not (parser, CPP_NAME))
	{
	  c_parser_error (parser, "expected identifier");
	  return;
	}
      superclass = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
    }
  else
    superclass = NULL_TREE;
  if (iface_p)
    {
      tree proto = NULL_TREE;
      if (c_parser_next_token_is (parser, CPP_LESS))
	proto = c_parser_objc_protocol_refs (parser);
      objc_start_class_interface (id1, superclass, proto, attributes);
    }
  else
    objc_start_class_implementation (id1, superclass);
  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    c_parser_objc_class_instance_variables (parser);
  if (iface_p)
    {
      objc_continue_interface ();
      c_parser_objc_methodprotolist (parser);
      c_parser_require_keyword (parser, RID_AT_END, "expected %<@end%>");
      objc_finish_interface ();
    }
  else
    {
      objc_continue_implementation ();
      return;
    }
}

/* Parse objc-class-instance-variables.

   objc-class-instance-variables:
     { objc-instance-variable-decl-list[opt] }

   objc-instance-variable-decl-list:
     objc-visibility-spec
     objc-instance-variable-decl ;
     ;
     objc-instance-variable-decl-list objc-visibility-spec
     objc-instance-variable-decl-list objc-instance-variable-decl ;
     objc-instance-variable-decl-list ;

   objc-visibility-spec:
     @private
     @protected
     @public

   objc-instance-variable-decl:
     struct-declaration
*/

static void
c_parser_objc_class_instance_variables (c_parser *parser)
{
  gcc_assert (c_parser_next_token_is (parser, CPP_OPEN_BRACE));
  c_parser_consume_token (parser);
  while (c_parser_next_token_is_not (parser, CPP_EOF))
    {
      tree decls;
      /* Parse any stray semicolon.  */
      if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	{
	  pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
		   "extra semicolon");
	  c_parser_consume_token (parser);
	  continue;
	}
      /* Stop if at the end of the instance variables.  */
      if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	{
	  c_parser_consume_token (parser);
	  break;
	}
      /* Parse any objc-visibility-spec.  */
      if (c_parser_next_token_is_keyword (parser, RID_AT_PRIVATE))
	{
	  c_parser_consume_token (parser);
	  objc_set_visibility (OBJC_IVAR_VIS_PRIVATE);
	  continue;
	}
      else if (c_parser_next_token_is_keyword (parser, RID_AT_PROTECTED))
	{
	  c_parser_consume_token (parser);
	  objc_set_visibility (OBJC_IVAR_VIS_PROTECTED);
	  continue;
	}
      else if (c_parser_next_token_is_keyword (parser, RID_AT_PUBLIC))
	{
	  c_parser_consume_token (parser);
	  objc_set_visibility (OBJC_IVAR_VIS_PUBLIC);
	  continue;
	}
      else if (c_parser_next_token_is_keyword (parser, RID_AT_PACKAGE))
	{
	  c_parser_consume_token (parser);
	  objc_set_visibility (OBJC_IVAR_VIS_PACKAGE);
	  continue;
	}
      else if (c_parser_next_token_is (parser, CPP_PRAGMA))
	{
	  c_parser_pragma (parser, pragma_external);
	  continue;
	}

      /* Parse some comma-separated declarations.  */
      decls = c_parser_struct_declaration (parser);
      if (decls == NULL)
	{
	  /* There is a syntax error.  We want to skip the offending
	     tokens up to the next ';' (included) or '}'
	     (excluded).  */
	  
	  /* First, skip manually a ')' or ']'.  This is because they
	     reduce the nesting level, so c_parser_skip_until_found()
	     wouldn't be able to skip past them.  */
	  c_token *token = c_parser_peek_token (parser);
	  if (token->type == CPP_CLOSE_PAREN || token->type == CPP_CLOSE_SQUARE)
	    c_parser_consume_token (parser);

	  /* Then, do the standard skipping.  */
	  c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);

	  /* We hopefully recovered.  Start normal parsing again.  */
	  parser->error = false;
	  continue;
	}
      else
	{
	  /* Comma-separated instance variables are chained together
	     in reverse order; add them one by one.  */
	  tree ivar = nreverse (decls);
	  for (; ivar; ivar = DECL_CHAIN (ivar))
	    objc_add_instance_variable (copy_node (ivar));
	}
      c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
    }
}

/* Parse an objc-class-declaration.

   objc-class-declaration:
     @class identifier-list ;
*/

static void
c_parser_objc_class_declaration (c_parser *parser)
{
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_CLASS));
  c_parser_consume_token (parser);
  /* Any identifiers, including those declared as type names, are OK
     here.  */
  while (true)
    {
      tree id;
      if (c_parser_next_token_is_not (parser, CPP_NAME))
	{
	  c_parser_error (parser, "expected identifier");
	  c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);
	  parser->error = false;
	  return;
	}
      id = c_parser_peek_token (parser)->value;
      objc_declare_class (id);
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
}

/* Parse an objc-alias-declaration.

   objc-alias-declaration:
     @compatibility_alias identifier identifier ;
*/

static void
c_parser_objc_alias_declaration (c_parser *parser)
{
  tree id1, id2;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_ALIAS));
  c_parser_consume_token (parser);
  if (c_parser_next_token_is_not (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected identifier");
      c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);
      return;
    }
  id1 = c_parser_peek_token (parser)->value;
  c_parser_consume_token (parser);
  if (c_parser_next_token_is_not (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected identifier");
      c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);
      return;
    }
  id2 = c_parser_peek_token (parser)->value;
  c_parser_consume_token (parser);
  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
  objc_declare_alias (id1, id2);
}

/* Parse an objc-protocol-definition.

   objc-protocol-definition:
     @protocol identifier objc-protocol-refs[opt] objc-methodprotolist @end
     @protocol identifier-list ;

   "@protocol identifier ;" should be resolved as "@protocol
   identifier-list ;": objc-methodprotolist may not start with a
   semicolon in the first alternative if objc-protocol-refs are
   omitted.  */

static void
c_parser_objc_protocol_definition (c_parser *parser, tree attributes)
{
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_PROTOCOL));

  c_parser_consume_token (parser);
  if (c_parser_next_token_is_not (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected identifier");
      return;
    }
  if (c_parser_peek_2nd_token (parser)->type == CPP_COMMA
      || c_parser_peek_2nd_token (parser)->type == CPP_SEMICOLON)
    {
      /* Any identifiers, including those declared as type names, are
	 OK here.  */
      while (true)
	{
	  tree id;
	  if (c_parser_next_token_is_not (parser, CPP_NAME))
	    {
	      c_parser_error (parser, "expected identifier");
	      break;
	    }
	  id = c_parser_peek_token (parser)->value;
	  objc_declare_protocol (id, attributes);
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    c_parser_consume_token (parser);
	  else
	    break;
	}
      c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
    }
  else
    {
      tree id = c_parser_peek_token (parser)->value;
      tree proto = NULL_TREE;
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_LESS))
	proto = c_parser_objc_protocol_refs (parser);
      parser->objc_pq_context = true;
      objc_start_protocol (id, proto, attributes);
      c_parser_objc_methodprotolist (parser);
      c_parser_require_keyword (parser, RID_AT_END, "expected %<@end%>");
      parser->objc_pq_context = false;
      objc_finish_interface ();
    }
}

/* Parse an objc-method-type.

   objc-method-type:
     +
     -

   Return true if it is a class method (+) and false if it is
   an instance method (-).
*/
static inline bool
c_parser_objc_method_type (c_parser *parser)
{
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_PLUS:
      c_parser_consume_token (parser);
      return true;
    case CPP_MINUS:
      c_parser_consume_token (parser);
      return false;
    default:
      gcc_unreachable ();
    }
}

/* Parse an objc-method-definition.

   objc-method-definition:
     objc-method-type objc-method-decl ;[opt] compound-statement
*/

static void
c_parser_objc_method_definition (c_parser *parser)
{
  bool is_class_method = c_parser_objc_method_type (parser);
  tree decl, attributes = NULL_TREE, expr = NULL_TREE;
  parser->objc_pq_context = true;
  decl = c_parser_objc_method_decl (parser, is_class_method, &attributes,
				    &expr);
  if (decl == error_mark_node)
    return;  /* Bail here. */

  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      c_parser_consume_token (parser);
      pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
	       "extra semicolon in method definition specified");
    }

  if (!c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    {
      c_parser_error (parser, "expected %<{%>");
      return;
    }

  parser->objc_pq_context = false;
  if (objc_start_method_definition (is_class_method, decl, attributes, expr))
    {
      add_stmt (c_parser_compound_statement (parser));
      objc_finish_method_definition (current_function_decl);
    }
  else
    {
      /* This code is executed when we find a method definition
	 outside of an @implementation context (or invalid for other
	 reasons).  Parse the method (to keep going) but do not emit
	 any code.
      */
      c_parser_compound_statement (parser);
    }
}

/* Parse an objc-methodprotolist.

   objc-methodprotolist:
     empty
     objc-methodprotolist objc-methodproto
     objc-methodprotolist declaration
     objc-methodprotolist ;
     @optional
     @required

   The declaration is a data definition, which may be missing
   declaration specifiers under the same rules and diagnostics as
   other data definitions outside functions, and the stray semicolon
   is diagnosed the same way as a stray semicolon outside a
   function.  */

static void
c_parser_objc_methodprotolist (c_parser *parser)
{
  while (true)
    {
      /* The list is terminated by @end.  */
      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_SEMICOLON:
	  pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
		   "ISO C does not allow extra %<;%> outside of a function");
	  c_parser_consume_token (parser);
	  break;
	case CPP_PLUS:
	case CPP_MINUS:
	  c_parser_objc_methodproto (parser);
	  break;
	case CPP_PRAGMA:
	  c_parser_pragma (parser, pragma_external);
	  break;
	case CPP_EOF:
	  return;
	default:
	  if (c_parser_next_token_is_keyword (parser, RID_AT_END))
	    return;
	  else if (c_parser_next_token_is_keyword (parser, RID_AT_PROPERTY))
	    c_parser_objc_at_property_declaration (parser);
	  else if (c_parser_next_token_is_keyword (parser, RID_AT_OPTIONAL))
	    {
	      objc_set_method_opt (true);
	      c_parser_consume_token (parser);
	    }
	  else if (c_parser_next_token_is_keyword (parser, RID_AT_REQUIRED))
	    {
	      objc_set_method_opt (false);
	      c_parser_consume_token (parser);
	    }
	  else
	    c_parser_declaration_or_fndef (parser, false, false, true,
					   false, true, NULL, vNULL);
	  break;
	}
    }
}

/* Parse an objc-methodproto.

   objc-methodproto:
     objc-method-type objc-method-decl ;
*/

static void
c_parser_objc_methodproto (c_parser *parser)
{
  bool is_class_method = c_parser_objc_method_type (parser);
  tree decl, attributes = NULL_TREE;

  /* Remember protocol qualifiers in prototypes.  */
  parser->objc_pq_context = true;
  decl = c_parser_objc_method_decl (parser, is_class_method, &attributes,
				    NULL);
  /* Forget protocol qualifiers now.  */
  parser->objc_pq_context = false;

  /* Do not allow the presence of attributes to hide an erroneous 
     method implementation in the interface section.  */
  if (!c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      c_parser_error (parser, "expected %<;%>");
      return;
    }
  
  if (decl != error_mark_node)
    objc_add_method_declaration (is_class_method, decl, attributes);

  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
}

/* If we are at a position that method attributes may be present, check that 
   there are not any parsed already (a syntax error) and then collect any 
   specified at the current location.  Finally, if new attributes were present,
   check that the next token is legal ( ';' for decls and '{' for defs).  */
   
static bool 
c_parser_objc_maybe_method_attributes (c_parser* parser, tree* attributes)
{
  bool bad = false;
  if (*attributes)
    {
      c_parser_error (parser, 
		    "method attributes must be specified at the end only");
      *attributes = NULL_TREE;
      bad = true;
    }

  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
    *attributes = c_parser_attributes (parser);

  /* If there were no attributes here, just report any earlier error.  */
  if (*attributes == NULL_TREE || bad)
    return bad;

  /* If the attributes are followed by a ; or {, then just report any earlier
     error.  */
  if (c_parser_next_token_is (parser, CPP_SEMICOLON)
      || c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    return bad;

  /* We've got attributes, but not at the end.  */
  c_parser_error (parser, 
		  "expected %<;%> or %<{%> after method attribute definition");
  return true;
}

/* Parse an objc-method-decl.

   objc-method-decl:
     ( objc-type-name ) objc-selector
     objc-selector
     ( objc-type-name ) objc-keyword-selector objc-optparmlist
     objc-keyword-selector objc-optparmlist
     attributes

   objc-keyword-selector:
     objc-keyword-decl
     objc-keyword-selector objc-keyword-decl

   objc-keyword-decl:
     objc-selector : ( objc-type-name ) identifier
     objc-selector : identifier
     : ( objc-type-name ) identifier
     : identifier

   objc-optparmlist:
     objc-optparms objc-optellipsis

   objc-optparms:
     empty
     objc-opt-parms , parameter-declaration

   objc-optellipsis:
     empty
     , ...
*/

static tree
c_parser_objc_method_decl (c_parser *parser, bool is_class_method,
			   tree *attributes, tree *expr)
{
  tree type = NULL_TREE;
  tree sel;
  tree parms = NULL_TREE;
  bool ellipsis = false;
  bool attr_err = false;

  *attributes = NULL_TREE;
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      c_parser_consume_token (parser);
      type = c_parser_objc_type_name (parser);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  sel = c_parser_objc_selector (parser);
  /* If there is no selector, or a colon follows, we have an
     objc-keyword-selector.  If there is a selector, and a colon does
     not follow, that selector ends the objc-method-decl.  */
  if (!sel || c_parser_next_token_is (parser, CPP_COLON))
    {
      tree tsel = sel;
      tree list = NULL_TREE;
      while (true)
	{
	  tree atype = NULL_TREE, id, keyworddecl;
	  tree param_attr = NULL_TREE;
	  if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	    break;
	  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
	    {
	      c_parser_consume_token (parser);
	      atype = c_parser_objc_type_name (parser);
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
					 "expected %<)%>");
	    }
	  /* New ObjC allows attributes on method parameters.  */
	  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	    param_attr = c_parser_attributes (parser);
	  if (c_parser_next_token_is_not (parser, CPP_NAME))
	    {
	      c_parser_error (parser, "expected identifier");
	      return error_mark_node;
	    }
	  id = c_parser_peek_token (parser)->value;
	  c_parser_consume_token (parser);
	  keyworddecl = objc_build_keyword_decl (tsel, atype, id, param_attr);
	  list = chainon (list, keyworddecl);
	  tsel = c_parser_objc_selector (parser);
	  if (!tsel && c_parser_next_token_is_not (parser, CPP_COLON))
	    break;
	}

      attr_err |= c_parser_objc_maybe_method_attributes (parser, attributes) ;

      /* Parse the optional parameter list.  Optional Objective-C
	 method parameters follow the C syntax, and may include '...'
	 to denote a variable number of arguments.  */
      parms = make_node (TREE_LIST);
      while (c_parser_next_token_is (parser, CPP_COMMA))
	{
	  struct c_parm *parm;
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
	    {
	      ellipsis = true;
	      c_parser_consume_token (parser);
	      attr_err |= c_parser_objc_maybe_method_attributes 
						(parser, attributes) ;
	      break;
	    }
	  parm = c_parser_parameter_declaration (parser, NULL_TREE);
	  if (parm == NULL)
	    break;
	  parms = chainon (parms,
			   build_tree_list (NULL_TREE, grokparm (parm, expr)));
	}
      sel = list;
    }
  else
    attr_err |= c_parser_objc_maybe_method_attributes (parser, attributes) ;

  if (sel == NULL)
    {
      c_parser_error (parser, "objective-c method declaration is expected");
      return error_mark_node;
    }

  if (attr_err)
    return error_mark_node;

  return objc_build_method_signature (is_class_method, type, sel, parms, ellipsis);
}

/* Parse an objc-type-name.

   objc-type-name:
     objc-type-qualifiers[opt] type-name
     objc-type-qualifiers[opt]

   objc-type-qualifiers:
     objc-type-qualifier
     objc-type-qualifiers objc-type-qualifier

   objc-type-qualifier: one of
     in out inout bycopy byref oneway
*/

static tree
c_parser_objc_type_name (c_parser *parser)
{
  tree quals = NULL_TREE;
  struct c_type_name *type_name = NULL;
  tree type = NULL_TREE;
  while (true)
    {
      c_token *token = c_parser_peek_token (parser);
      if (token->type == CPP_KEYWORD
	  && (token->keyword == RID_IN
	      || token->keyword == RID_OUT
	      || token->keyword == RID_INOUT
	      || token->keyword == RID_BYCOPY
	      || token->keyword == RID_BYREF
	      || token->keyword == RID_ONEWAY))
	{
	  quals = chainon (build_tree_list (NULL_TREE, token->value), quals);
	  c_parser_consume_token (parser);
	}
      else
	break;
    }
  if (c_parser_next_tokens_start_typename (parser, cla_prefer_type))
    type_name = c_parser_type_name (parser);
  if (type_name)
    type = groktypename (type_name, NULL, NULL);

  /* If the type is unknown, and error has already been produced and
     we need to recover from the error.  In that case, use NULL_TREE
     for the type, as if no type had been specified; this will use the
     default type ('id') which is good for error recovery.  */
  if (type == error_mark_node)
    type = NULL_TREE;

  return build_tree_list (quals, type);
}

/* Parse objc-protocol-refs.

   objc-protocol-refs:
     < identifier-list >
*/

static tree
c_parser_objc_protocol_refs (c_parser *parser)
{
  tree list = NULL_TREE;
  gcc_assert (c_parser_next_token_is (parser, CPP_LESS));
  c_parser_consume_token (parser);
  /* Any identifiers, including those declared as type names, are OK
     here.  */
  while (true)
    {
      tree id;
      if (c_parser_next_token_is_not (parser, CPP_NAME))
	{
	  c_parser_error (parser, "expected identifier");
	  break;
	}
      id = c_parser_peek_token (parser)->value;
      list = chainon (list, build_tree_list (NULL_TREE, id));
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  c_parser_require (parser, CPP_GREATER, "expected %<>%>");
  return list;
}

/* Parse an objc-try-catch-finally-statement.

   objc-try-catch-finally-statement:
     @try compound-statement objc-catch-list[opt]
     @try compound-statement objc-catch-list[opt] @finally compound-statement

   objc-catch-list:
     @catch ( objc-catch-parameter-declaration ) compound-statement
     objc-catch-list @catch ( objc-catch-parameter-declaration ) compound-statement

   objc-catch-parameter-declaration:
     parameter-declaration
     '...'

   where '...' is to be interpreted literally, that is, it means CPP_ELLIPSIS.

   PS: This function is identical to cp_parser_objc_try_catch_finally_statement
   for C++.  Keep them in sync.  */   

static void
c_parser_objc_try_catch_finally_statement (c_parser *parser)
{
  location_t location;
  tree stmt;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_TRY));
  c_parser_consume_token (parser);
  location = c_parser_peek_token (parser)->location;
  objc_maybe_warn_exceptions (location);
  stmt = c_parser_compound_statement (parser);
  objc_begin_try_stmt (location, stmt);

  while (c_parser_next_token_is_keyword (parser, RID_AT_CATCH))
    {
      struct c_parm *parm;
      tree parameter_declaration = error_mark_node;
      bool seen_open_paren = false;

      c_parser_consume_token (parser);
      if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	seen_open_paren = true;
      if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
	{
	  /* We have "@catch (...)" (where the '...' are literally
	     what is in the code).  Skip the '...'.
	     parameter_declaration is set to NULL_TREE, and
	     objc_being_catch_clauses() knows that that means
	     '...'.  */
	  c_parser_consume_token (parser);
	  parameter_declaration = NULL_TREE;
	}
      else
	{
	  /* We have "@catch (NSException *exception)" or something
	     like that.  Parse the parameter declaration.  */
	  parm = c_parser_parameter_declaration (parser, NULL_TREE);
	  if (parm == NULL)
	    parameter_declaration = error_mark_node;
	  else
	    parameter_declaration = grokparm (parm, NULL);
	}
      if (seen_open_paren)
	c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>");
      else
	{
	  /* If there was no open parenthesis, we are recovering from
	     an error, and we are trying to figure out what mistake
	     the user has made.  */

	  /* If there is an immediate closing parenthesis, the user
	     probably forgot the opening one (ie, they typed "@catch
	     NSException *e)".  Parse the closing parenthesis and keep
	     going.  */
	  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    c_parser_consume_token (parser);
	  
	  /* If these is no immediate closing parenthesis, the user
	     probably doesn't know that parenthesis are required at
	     all (ie, they typed "@catch NSException *e").  So, just
	     forget about the closing parenthesis and keep going.  */
	}
      objc_begin_catch_clause (parameter_declaration);
      if (c_parser_require (parser, CPP_OPEN_BRACE, "expected %<{%>"))
	c_parser_compound_statement_nostart (parser);
      objc_finish_catch_clause ();
    }
  if (c_parser_next_token_is_keyword (parser, RID_AT_FINALLY))
    {
      c_parser_consume_token (parser);
      location = c_parser_peek_token (parser)->location;
      stmt = c_parser_compound_statement (parser);
      objc_build_finally_clause (location, stmt);
    }
  objc_finish_try_stmt ();
}

/* Parse an objc-synchronized-statement.

   objc-synchronized-statement:
     @synchronized ( expression ) compound-statement
*/

static void
c_parser_objc_synchronized_statement (c_parser *parser)
{
  location_t loc;
  tree expr, stmt;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_SYNCHRONIZED));
  c_parser_consume_token (parser);
  loc = c_parser_peek_token (parser)->location;
  objc_maybe_warn_exceptions (loc);
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      struct c_expr ce = c_parser_expression (parser);
      ce = convert_lvalue_to_rvalue (loc, ce, false, false);
      expr = ce.value;
      expr = c_fully_fold (expr, false, NULL);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  else
    expr = error_mark_node;
  stmt = c_parser_compound_statement (parser);
  objc_build_synchronized (loc, expr, stmt);
}

/* Parse an objc-selector; return NULL_TREE without an error if the
   next token is not an objc-selector.

   objc-selector:
     identifier
     one of
       enum struct union if else while do for switch case default
       break continue return goto asm sizeof typeof __alignof
       unsigned long const short volatile signed restrict _Complex
       in out inout bycopy byref oneway int char float double void _Bool
       _Atomic

   ??? Why this selection of keywords but not, for example, storage
   class specifiers?  */

static tree
c_parser_objc_selector (c_parser *parser)
{
  c_token *token = c_parser_peek_token (parser);
  tree value = token->value;
  if (token->type == CPP_NAME)
    {
      c_parser_consume_token (parser);
      return value;
    }
  if (token->type != CPP_KEYWORD)
    return NULL_TREE;
  switch (token->keyword)
    {
    case RID_ENUM:
    case RID_STRUCT:
    case RID_UNION:
    case RID_IF:
    case RID_ELSE:
    case RID_WHILE:
    case RID_DO:
    case RID_FOR:
    case RID_SWITCH:
    case RID_CASE:
    case RID_DEFAULT:
    case RID_BREAK:
    case RID_CONTINUE:
    case RID_RETURN:
    case RID_GOTO:
    case RID_ASM:
    case RID_SIZEOF:
    case RID_TYPEOF:
    case RID_ALIGNOF:
    case RID_UNSIGNED:
    case RID_LONG:
    case RID_CONST:
    case RID_SHORT:
    case RID_VOLATILE:
    case RID_SIGNED:
    case RID_RESTRICT:
    case RID_COMPLEX:
    case RID_IN:
    case RID_OUT:
    case RID_INOUT:
    case RID_BYCOPY:
    case RID_BYREF:
    case RID_ONEWAY:
    case RID_INT:
    case RID_CHAR:
    case RID_FLOAT:
    case RID_DOUBLE:
    case RID_VOID:
    case RID_BOOL:
    case RID_ATOMIC:
    case RID_AUTO_TYPE:
    case RID_INT_N_0:
    case RID_INT_N_1:
    case RID_INT_N_2:
    case RID_INT_N_3:
      c_parser_consume_token (parser);
      return value;
    default:
      return NULL_TREE;
    }
}

/* Parse an objc-selector-arg.

   objc-selector-arg:
     objc-selector
     objc-keywordname-list

   objc-keywordname-list:
     objc-keywordname
     objc-keywordname-list objc-keywordname

   objc-keywordname:
     objc-selector :
     :
*/

static tree
c_parser_objc_selector_arg (c_parser *parser)
{
  tree sel = c_parser_objc_selector (parser);
  tree list = NULL_TREE;
  if (sel && c_parser_next_token_is_not (parser, CPP_COLON))
    return sel;
  while (true)
    {
      if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	return list;
      list = chainon (list, build_tree_list (sel, NULL_TREE));
      sel = c_parser_objc_selector (parser);
      if (!sel && c_parser_next_token_is_not (parser, CPP_COLON))
	break;
    }
  return list;
}

/* Parse an objc-receiver.

   objc-receiver:
     expression
     class-name
     type-name
*/

static tree
c_parser_objc_receiver (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;

  if (c_parser_peek_token (parser)->type == CPP_NAME
      && (c_parser_peek_token (parser)->id_kind == C_ID_TYPENAME
	  || c_parser_peek_token (parser)->id_kind == C_ID_CLASSNAME))
    {
      tree id = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
      return objc_get_class_reference (id);
    }
  struct c_expr ce = c_parser_expression (parser);
  ce = convert_lvalue_to_rvalue (loc, ce, false, false);
  return c_fully_fold (ce.value, false, NULL);
}

/* Parse objc-message-args.

   objc-message-args:
     objc-selector
     objc-keywordarg-list

   objc-keywordarg-list:
     objc-keywordarg
     objc-keywordarg-list objc-keywordarg

   objc-keywordarg:
     objc-selector : objc-keywordexpr
     : objc-keywordexpr
*/

static tree
c_parser_objc_message_args (c_parser *parser)
{
  tree sel = c_parser_objc_selector (parser);
  tree list = NULL_TREE;
  if (sel && c_parser_next_token_is_not (parser, CPP_COLON))
    return sel;
  while (true)
    {
      tree keywordexpr;
      if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	return error_mark_node;
      keywordexpr = c_parser_objc_keywordexpr (parser);
      list = chainon (list, build_tree_list (sel, keywordexpr));
      sel = c_parser_objc_selector (parser);
      if (!sel && c_parser_next_token_is_not (parser, CPP_COLON))
	break;
    }
  return list;
}

/* Parse an objc-keywordexpr.

   objc-keywordexpr:
     nonempty-expr-list
*/

static tree
c_parser_objc_keywordexpr (c_parser *parser)
{
  tree ret;
  vec<tree, va_gc> *expr_list = c_parser_expr_list (parser, true, true,
						NULL, NULL, NULL, NULL);
  if (vec_safe_length (expr_list) == 1)
    {
      /* Just return the expression, remove a level of
	 indirection.  */
      ret = (*expr_list)[0];
    }
  else
    {
      /* We have a comma expression, we will collapse later.  */
      ret = build_tree_list_vec (expr_list);
    }
  release_tree_vector (expr_list);
  return ret;
}

/* A check, needed in several places, that ObjC interface, implementation or
   method definitions are not prefixed by incorrect items.  */
static bool
c_parser_objc_diagnose_bad_element_prefix (c_parser *parser, 
					   struct c_declspecs *specs)
{
  if (!specs->declspecs_seen_p || specs->non_sc_seen_p
      || specs->typespec_kind != ctsk_none)
    {
      c_parser_error (parser, 
      		      "no type or storage class may be specified here,");
      c_parser_skip_to_end_of_block_or_statement (parser);
      return true;
    }
  return false;
}

/* Parse an Objective-C @property declaration.  The syntax is:

   objc-property-declaration:
     '@property' objc-property-attributes[opt] struct-declaration ;

   objc-property-attributes:
    '(' objc-property-attribute-list ')'

   objc-property-attribute-list:
     objc-property-attribute
     objc-property-attribute-list, objc-property-attribute

   objc-property-attribute
     'getter' = identifier
     'setter' = identifier
     'readonly'
     'readwrite'
     'assign'
     'retain'
     'copy'
     'nonatomic'

  For example:
    @property NSString *name;
    @property (readonly) id object;
    @property (retain, nonatomic, getter=getTheName) id name;
    @property int a, b, c;

  PS: This function is identical to cp_parser_objc_at_propery_declaration
  for C++.  Keep them in sync.  */
static void
c_parser_objc_at_property_declaration (c_parser *parser)
{
  /* The following variables hold the attributes of the properties as
     parsed.  They are 'false' or 'NULL_TREE' if the attribute was not
     seen.  When we see an attribute, we set them to 'true' (if they
     are boolean properties) or to the identifier (if they have an
     argument, ie, for getter and setter).  Note that here we only
     parse the list of attributes, check the syntax and accumulate the
     attributes that we find.  objc_add_property_declaration() will
     then process the information.  */
  bool property_assign = false;
  bool property_copy = false;
  tree property_getter_ident = NULL_TREE;
  bool property_nonatomic = false;
  bool property_readonly = false;
  bool property_readwrite = false;
  bool property_retain = false;
  tree property_setter_ident = NULL_TREE;

  /* 'properties' is the list of properties that we read.  Usually a
     single one, but maybe more (eg, in "@property int a, b, c;" there
     are three).  */
  tree properties;
  location_t loc;

  loc = c_parser_peek_token (parser)->location;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_PROPERTY));

  c_parser_consume_token (parser);  /* Eat '@property'.  */

  /* Parse the optional attribute list...  */
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      /* Eat the '(' */
      c_parser_consume_token (parser);
      
      /* Property attribute keywords are valid now.  */
      parser->objc_property_attr_context = true;

      while (true)
	{
	  bool syntax_error = false;
	  c_token *token = c_parser_peek_token (parser);
	  enum rid keyword;

	  if (token->type != CPP_KEYWORD)
	    {
	      if (token->type == CPP_CLOSE_PAREN)
		c_parser_error (parser, "expected identifier");
	      else
		{
		  c_parser_consume_token (parser);
		  c_parser_error (parser, "unknown property attribute");
		}
	      break;
	    }
	  keyword = token->keyword;
	  c_parser_consume_token (parser);
	  switch (keyword)
	    {
	    case RID_ASSIGN:    property_assign = true;    break;
	    case RID_COPY:      property_copy = true;      break;
	    case RID_NONATOMIC: property_nonatomic = true; break;
	    case RID_READONLY:  property_readonly = true;  break;
	    case RID_READWRITE: property_readwrite = true; break;
	    case RID_RETAIN:    property_retain = true;    break;

	    case RID_GETTER:
	    case RID_SETTER:
	      if (c_parser_next_token_is_not (parser, CPP_EQ))
		{
		  if (keyword == RID_GETTER)
		    c_parser_error (parser,
				    "missing %<=%> (after %<getter%> attribute)");
		  else
		    c_parser_error (parser,
				    "missing %<=%> (after %<setter%> attribute)");
		  syntax_error = true;
		  break;
		}
	      c_parser_consume_token (parser); /* eat the = */
	      if (c_parser_next_token_is_not (parser, CPP_NAME))
		{
		  c_parser_error (parser, "expected identifier");
		  syntax_error = true;
		  break;
		}
	      if (keyword == RID_SETTER)
		{
		  if (property_setter_ident != NULL_TREE)
		    c_parser_error (parser, "the %<setter%> attribute may only be specified once");
		  else
		    property_setter_ident = c_parser_peek_token (parser)->value;
		  c_parser_consume_token (parser);
		  if (c_parser_next_token_is_not (parser, CPP_COLON))
		    c_parser_error (parser, "setter name must terminate with %<:%>");
		  else
		    c_parser_consume_token (parser);
		}
	      else
		{
		  if (property_getter_ident != NULL_TREE)
		    c_parser_error (parser, "the %<getter%> attribute may only be specified once");
		  else
		    property_getter_ident = c_parser_peek_token (parser)->value;
		  c_parser_consume_token (parser);
		}
	      break;
	    default:
	      c_parser_error (parser, "unknown property attribute");
	      syntax_error = true;
	      break;
	    }

	  if (syntax_error)
	    break;
	  
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    c_parser_consume_token (parser);
	  else
	    break;
	}
      parser->objc_property_attr_context = false;
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  /* ... and the property declaration(s).  */
  properties = c_parser_struct_declaration (parser);

  if (properties == error_mark_node)
    {
      c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);
      parser->error = false;
      return;
    }

  if (properties == NULL_TREE)
    c_parser_error (parser, "expected identifier");
  else
    {
      /* Comma-separated properties are chained together in
	 reverse order; add them one by one.  */
      properties = nreverse (properties);
      
      for (; properties; properties = TREE_CHAIN (properties))
	objc_add_property_declaration (loc, copy_node (properties),
				       property_readonly, property_readwrite,
				       property_assign, property_retain,
				       property_copy, property_nonatomic,
				       property_getter_ident, property_setter_ident);
    }

  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
  parser->error = false;
}

/* Parse an Objective-C @synthesize declaration.  The syntax is:

   objc-synthesize-declaration:
     @synthesize objc-synthesize-identifier-list ;

   objc-synthesize-identifier-list:
     objc-synthesize-identifier
     objc-synthesize-identifier-list, objc-synthesize-identifier

   objc-synthesize-identifier
     identifier
     identifier = identifier

  For example:
    @synthesize MyProperty;
    @synthesize OneProperty, AnotherProperty=MyIvar, YetAnotherProperty;

  PS: This function is identical to cp_parser_objc_at_synthesize_declaration
  for C++.  Keep them in sync.
*/
static void
c_parser_objc_at_synthesize_declaration (c_parser *parser)
{
  tree list = NULL_TREE;
  location_t loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_SYNTHESIZE));
  loc = c_parser_peek_token (parser)->location;

  c_parser_consume_token (parser);
  while (true)
    {
      tree property, ivar;
      if (c_parser_next_token_is_not (parser, CPP_NAME))
	{
	  c_parser_error (parser, "expected identifier");
	  c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);
	  /* Once we find the semicolon, we can resume normal parsing.
	     We have to reset parser->error manually because
	     c_parser_skip_until_found() won't reset it for us if the
	     next token is precisely a semicolon.  */
	  parser->error = false;
	  return;
	}
      property = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_EQ))
	{
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is_not (parser, CPP_NAME))
	    {
	      c_parser_error (parser, "expected identifier");
	      c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);
	      parser->error = false;
	      return;
	    }
	  ivar = c_parser_peek_token (parser)->value;
	  c_parser_consume_token (parser);
	}
      else
	ivar = NULL_TREE;
      list = chainon (list, build_tree_list (ivar, property));
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
  objc_add_synthesize_declaration (loc, list);
}

/* Parse an Objective-C @dynamic declaration.  The syntax is:

   objc-dynamic-declaration:
     @dynamic identifier-list ;

   For example:
     @dynamic MyProperty;
     @dynamic MyProperty, AnotherProperty;

  PS: This function is identical to cp_parser_objc_at_dynamic_declaration
  for C++.  Keep them in sync.
*/
static void
c_parser_objc_at_dynamic_declaration (c_parser *parser)
{
  tree list = NULL_TREE;
  location_t loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_AT_DYNAMIC));
  loc = c_parser_peek_token (parser)->location;

  c_parser_consume_token (parser);
  while (true)
    {
      tree property;
      if (c_parser_next_token_is_not (parser, CPP_NAME))
	{
	  c_parser_error (parser, "expected identifier");
	  c_parser_skip_until_found (parser, CPP_SEMICOLON, NULL);
	  parser->error = false;
	  return;
	}
      property = c_parser_peek_token (parser)->value;
      list = chainon (list, build_tree_list (NULL_TREE, property));
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
  objc_add_dynamic_declaration (loc, list);
}


/* Handle pragmas.  Some OpenMP pragmas are associated with, and therefore
   should be considered, statements.  ALLOW_STMT is true if we're within
   the context of a function and such pragmas are to be allowed.  Returns
   true if we actually parsed such a pragma.  */

static bool
c_parser_pragma (c_parser *parser, enum pragma_context context)
{
  unsigned int id;

  id = c_parser_peek_token (parser)->pragma_kind;
  gcc_assert (id != PRAGMA_NONE);

  switch (id)
    {
    case PRAGMA_OMP_BARRIER:
      if (context != pragma_compound)
	{
	  if (context == pragma_stmt)
	    c_parser_error (parser, "%<#pragma omp barrier%> may only be "
			    "used in compound statements");
	  goto bad_stmt;
	}
      c_parser_omp_barrier (parser);
      return false;

    case PRAGMA_OMP_FLUSH:
      if (context != pragma_compound)
	{
	  if (context == pragma_stmt)
	    c_parser_error (parser, "%<#pragma omp flush%> may only be "
			    "used in compound statements");
	  goto bad_stmt;
	}
      c_parser_omp_flush (parser);
      return false;

    case PRAGMA_OMP_TASKWAIT:
      if (context != pragma_compound)
	{
	  if (context == pragma_stmt)
	    c_parser_error (parser, "%<#pragma omp taskwait%> may only be "
			    "used in compound statements");
	  goto bad_stmt;
	}
      c_parser_omp_taskwait (parser);
      return false;

    case PRAGMA_OMP_TASKYIELD:
      if (context != pragma_compound)
	{
	  if (context == pragma_stmt)
	    c_parser_error (parser, "%<#pragma omp taskyield%> may only be "
			    "used in compound statements");
	  goto bad_stmt;
	}
      c_parser_omp_taskyield (parser);
      return false;

    case PRAGMA_OMP_CANCEL:
      if (context != pragma_compound)
	{
	  if (context == pragma_stmt)
	    c_parser_error (parser, "%<#pragma omp cancel%> may only be "
			    "used in compound statements");
	  goto bad_stmt;
	}
      c_parser_omp_cancel (parser);
      return false;

    case PRAGMA_OMP_CANCELLATION_POINT:
      if (context != pragma_compound)
	{
	  if (context == pragma_stmt)
	    c_parser_error (parser, "%<#pragma omp cancellation point%> may "
				    "only be used in compound statements");
	  goto bad_stmt;
	}
      c_parser_omp_cancellation_point (parser);
      return false;

    case PRAGMA_OMP_THREADPRIVATE:
      c_parser_omp_threadprivate (parser);
      return false;

    case PRAGMA_OMP_TARGET:
      return c_parser_omp_target (parser, context);

    case PRAGMA_OMP_END_DECLARE_TARGET:
      c_parser_omp_end_declare_target (parser);
      return false;

    case PRAGMA_OMP_SECTION:
      error_at (c_parser_peek_token (parser)->location,
		"%<#pragma omp section%> may only be used in "
		"%<#pragma omp sections%> construct");
      c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
      return false;

    case PRAGMA_OMP_DECLARE_REDUCTION:
      c_parser_omp_declare (parser, context);
      return false;
    case PRAGMA_IVDEP:
      c_parser_consume_pragma (parser);
      c_parser_skip_to_pragma_eol (parser);
      if (!c_parser_next_token_is_keyword (parser, RID_FOR)
	  && !c_parser_next_token_is_keyword (parser, RID_WHILE)
	  && !c_parser_next_token_is_keyword (parser, RID_DO))
	{
	  c_parser_error (parser, "for, while or do statement expected");
	  return false;
	}
      if (c_parser_next_token_is_keyword (parser, RID_FOR))
	c_parser_for_statement (parser, true);
      else if (c_parser_next_token_is_keyword (parser, RID_WHILE))
	c_parser_while_statement (parser, true);
      else
	c_parser_do_statement (parser, true);
      return false;

    case PRAGMA_GCC_PCH_PREPROCESS:
      c_parser_error (parser, "%<#pragma GCC pch_preprocess%> must be first");
      c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
      return false;

    case PRAGMA_CILK_SIMD:
      if (!c_parser_cilk_verify_simd (parser, context))
	return false;
      c_parser_consume_pragma (parser);
      c_parser_cilk_simd (parser);
      return false;
    case PRAGMA_CILK_GRAINSIZE:
      if (!flag_cilkplus)
	{
	  warning (0, "%<#pragma grainsize%> ignored because -fcilkplus is not"
		   " enabled");
	  c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
	  return false;
	}
      if (context == pragma_external)
	{
	  error_at (c_parser_peek_token (parser)->location,
		    "%<#pragma grainsize%> must be inside a function");
	  c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
	  return false;
	}
      c_parser_cilk_grainsize (parser);
      return false;

    default:
      if (id < PRAGMA_FIRST_EXTERNAL)
	{
	  if (context != pragma_stmt && context != pragma_compound)
	    {
	    bad_stmt:
	      c_parser_error (parser, "expected declaration specifiers");
	      c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
	      return false;
	    }
	  c_parser_omp_construct (parser);
	  return true;
	}
      break;
    }

  c_parser_consume_pragma (parser);
  c_invoke_pragma_handler (id);

  /* Skip to EOL, but suppress any error message.  Those will have been
     generated by the handler routine through calling error, as opposed
     to calling c_parser_error.  */
  parser->error = true;
  c_parser_skip_to_pragma_eol (parser);

  return false;
}

/* The interface the pragma parsers have to the lexer.  */

enum cpp_ttype
pragma_lex (tree *value)
{
  c_token *tok = c_parser_peek_token (the_parser);
  enum cpp_ttype ret = tok->type;

  *value = tok->value;
  if (ret == CPP_PRAGMA_EOL || ret == CPP_EOF)
    ret = CPP_EOF;
  else
    {
      if (ret == CPP_KEYWORD)
	ret = CPP_NAME;
      c_parser_consume_token (the_parser);
    }

  return ret;
}

static void
c_parser_pragma_pch_preprocess (c_parser *parser)
{
  tree name = NULL;

  c_parser_consume_pragma (parser);
  if (c_parser_next_token_is (parser, CPP_STRING))
    {
      name = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
    }
  else
    c_parser_error (parser, "expected string literal");
  c_parser_skip_to_pragma_eol (parser);

  if (name)
    c_common_pch_pragma (parse_in, TREE_STRING_POINTER (name));
}

/* OpenMP 2.5 / 3.0 / 3.1 / 4.0 parsing routines.  */

/* Returns name of the next clause.
   If the clause is not recognized PRAGMA_OMP_CLAUSE_NONE is returned and
   the token is not consumed.  Otherwise appropriate pragma_omp_clause is
   returned and the token is consumed.  */

static pragma_omp_clause
c_parser_omp_clause_name (c_parser *parser)
{
  pragma_omp_clause result = PRAGMA_OMP_CLAUSE_NONE;

  if (c_parser_next_token_is_keyword (parser, RID_IF))
    result = PRAGMA_OMP_CLAUSE_IF;
  else if (c_parser_next_token_is_keyword (parser, RID_DEFAULT))
    result = PRAGMA_OMP_CLAUSE_DEFAULT;
  else if (c_parser_next_token_is_keyword (parser, RID_FOR))
    result = PRAGMA_OMP_CLAUSE_FOR;
  else if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);

      switch (p[0])
	{
	case 'a':
	  if (!strcmp ("aligned", p))
	    result = PRAGMA_OMP_CLAUSE_ALIGNED;
	  break;
	case 'c':
	  if (!strcmp ("collapse", p))
	    result = PRAGMA_OMP_CLAUSE_COLLAPSE;
	  else if (!strcmp ("copyin", p))
	    result = PRAGMA_OMP_CLAUSE_COPYIN;
          else if (!strcmp ("copyprivate", p))
	    result = PRAGMA_OMP_CLAUSE_COPYPRIVATE;
	  break;
	case 'd':
	  if (!strcmp ("depend", p))
	    result = PRAGMA_OMP_CLAUSE_DEPEND;
	  else if (!strcmp ("device", p))
	    result = PRAGMA_OMP_CLAUSE_DEVICE;
	  else if (!strcmp ("dist_schedule", p))
	    result = PRAGMA_OMP_CLAUSE_DIST_SCHEDULE;
	  break;
	case 'f':
	  if (!strcmp ("final", p))
	    result = PRAGMA_OMP_CLAUSE_FINAL;
	  else if (!strcmp ("firstprivate", p))
	    result = PRAGMA_OMP_CLAUSE_FIRSTPRIVATE;
	  else if (!strcmp ("from", p))
	    result = PRAGMA_OMP_CLAUSE_FROM;
	  break;
	case 'i':
	  if (!strcmp ("inbranch", p))
	    result = PRAGMA_OMP_CLAUSE_INBRANCH;
	  break;
	case 'l':
	  if (!strcmp ("lastprivate", p))
	    result = PRAGMA_OMP_CLAUSE_LASTPRIVATE;
	  else if (!strcmp ("linear", p))
	    result = PRAGMA_OMP_CLAUSE_LINEAR;
	  break;
	case 'm':
	  if (!strcmp ("map", p))
	    result = PRAGMA_OMP_CLAUSE_MAP;
	  else if (!strcmp ("mergeable", p))
	    result = PRAGMA_OMP_CLAUSE_MERGEABLE;
	  else if (flag_cilkplus && !strcmp ("mask", p))
	    result = PRAGMA_CILK_CLAUSE_MASK;
	  break;
	case 'n':
	  if (!strcmp ("notinbranch", p))
	    result = PRAGMA_OMP_CLAUSE_NOTINBRANCH;
	  else if (!strcmp ("nowait", p))
	    result = PRAGMA_OMP_CLAUSE_NOWAIT;
	  else if (!strcmp ("num_teams", p))
	    result = PRAGMA_OMP_CLAUSE_NUM_TEAMS;
	  else if (!strcmp ("num_threads", p))
	    result = PRAGMA_OMP_CLAUSE_NUM_THREADS;
	  else if (flag_cilkplus && !strcmp ("nomask", p))
	    result = PRAGMA_CILK_CLAUSE_NOMASK;
	  break;
	case 'o':
	  if (!strcmp ("ordered", p))
	    result = PRAGMA_OMP_CLAUSE_ORDERED;
	  break;
	case 'p':
	  if (!strcmp ("parallel", p))
	    result = PRAGMA_OMP_CLAUSE_PARALLEL;
	  else if (!strcmp ("private", p))
	    result = PRAGMA_OMP_CLAUSE_PRIVATE;
	  else if (!strcmp ("proc_bind", p))
	    result = PRAGMA_OMP_CLAUSE_PROC_BIND;
	  break;
	case 'r':
	  if (!strcmp ("reduction", p))
	    result = PRAGMA_OMP_CLAUSE_REDUCTION;
	  break;
	case 's':
	  if (!strcmp ("safelen", p))
	    result = PRAGMA_OMP_CLAUSE_SAFELEN;
	  else if (!strcmp ("schedule", p))
	    result = PRAGMA_OMP_CLAUSE_SCHEDULE;
	  else if (!strcmp ("sections", p))
	    result = PRAGMA_OMP_CLAUSE_SECTIONS;
	  else if (!strcmp ("shared", p))
	    result = PRAGMA_OMP_CLAUSE_SHARED;
	  else if (!strcmp ("simdlen", p))
	    result = PRAGMA_OMP_CLAUSE_SIMDLEN;
	  break;
	case 't':
	  if (!strcmp ("taskgroup", p))
	    result = PRAGMA_OMP_CLAUSE_TASKGROUP;
	  else if (!strcmp ("thread_limit", p))
	    result = PRAGMA_OMP_CLAUSE_THREAD_LIMIT;
	  else if (!strcmp ("to", p))
	    result = PRAGMA_OMP_CLAUSE_TO;
	  break;
	case 'u':
	  if (!strcmp ("uniform", p))
	    result = PRAGMA_OMP_CLAUSE_UNIFORM;
	  else if (!strcmp ("untied", p))
	    result = PRAGMA_OMP_CLAUSE_UNTIED;
	  break;
	case 'v':
	  if (flag_cilkplus && !strcmp ("vectorlength", p))
	    result = PRAGMA_CILK_CLAUSE_VECTORLENGTH;
	  break;
	}
    }

  if (result != PRAGMA_OMP_CLAUSE_NONE)
    c_parser_consume_token (parser);

  return result;
}

/* Validate that a clause of the given type does not already exist.  */

static void
check_no_duplicate_clause (tree clauses, enum omp_clause_code code,
			   const char *name)
{
  tree c;

  for (c = clauses; c ; c = OMP_CLAUSE_CHAIN (c))
    if (OMP_CLAUSE_CODE (c) == code)
      {
	location_t loc = OMP_CLAUSE_LOCATION (c);
	error_at (loc, "too many %qs clauses", name);
	break;
      }
}

/* OpenMP 2.5:
   variable-list:
     identifier
     variable-list , identifier

   If KIND is nonzero, create the appropriate node and install the
   decl in OMP_CLAUSE_DECL and add the node to the head of the list.
   If KIND is nonzero, CLAUSE_LOC is the location of the clause.

   If KIND is zero, create a TREE_LIST with the decl in TREE_PURPOSE;
   return the list created.  */

static tree
c_parser_omp_variable_list (c_parser *parser,
			    location_t clause_loc,
			    enum omp_clause_code kind, tree list)
{
  if (c_parser_next_token_is_not (parser, CPP_NAME)
      || c_parser_peek_token (parser)->id_kind != C_ID_ID)
    c_parser_error (parser, "expected identifier");

  while (c_parser_next_token_is (parser, CPP_NAME)
	 && c_parser_peek_token (parser)->id_kind == C_ID_ID)
    {
      tree t = lookup_name (c_parser_peek_token (parser)->value);

      if (t == NULL_TREE)
	{
	  undeclared_variable (c_parser_peek_token (parser)->location,
			       c_parser_peek_token (parser)->value);
	  t = error_mark_node;
	}

      c_parser_consume_token (parser);

      if (t == error_mark_node)
	;
      else if (kind != 0)
	{
	  switch (kind)
	    {
	    case OMP_CLAUSE_MAP:
	    case OMP_CLAUSE_FROM:
	    case OMP_CLAUSE_TO:
	    case OMP_CLAUSE_DEPEND:
	      while (c_parser_next_token_is (parser, CPP_OPEN_SQUARE))
		{
		  tree low_bound = NULL_TREE, length = NULL_TREE;

		  c_parser_consume_token (parser);
		  if (!c_parser_next_token_is (parser, CPP_COLON))
		    {
		      low_bound = c_parser_expression (parser).value;
		      mark_exp_read (low_bound);
		    }
		  if (c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
		    length = integer_one_node;
		  else
		    {
		      /* Look for `:'.  */
		      if (!c_parser_require (parser, CPP_COLON,
					     "expected %<:%>"))
			{
			  t = error_mark_node;
			  break;
			}
		      if (!c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
			{
			  length = c_parser_expression (parser).value;
			  mark_exp_read (length);
			}
		    }
		  /* Look for the closing `]'.  */
		  if (!c_parser_require (parser, CPP_CLOSE_SQUARE,
					 "expected %<]%>"))
		    {
		      t = error_mark_node;
		      break;
		    }
		  t = tree_cons (low_bound, length, t);
		}
	      break;
	    default:
	      break;
	    }

	  if (t != error_mark_node)
	    {
	      tree u = build_omp_clause (clause_loc, kind);
	      OMP_CLAUSE_DECL (u) = t;
	      OMP_CLAUSE_CHAIN (u) = list;
	      list = u;
	    }
	}
      else
	list = tree_cons (t, NULL_TREE, list);

      if (c_parser_next_token_is_not (parser, CPP_COMMA))
	break;

      c_parser_consume_token (parser);
    }

  return list;
}

/* Similarly, but expect leading and trailing parenthesis.  This is a very
   common case for omp clauses.  */

static tree
c_parser_omp_var_list_parens (c_parser *parser, enum omp_clause_code kind,
			      tree list)
{
  /* The clauses location.  */
  location_t loc = c_parser_peek_token (parser)->location;

  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      list = c_parser_omp_variable_list (parser, loc, kind, list);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  return list;
}

/* OpenMP 3.0:
   collapse ( constant-expression ) */

static tree
c_parser_omp_clause_collapse (c_parser *parser, tree list)
{
  tree c, num = error_mark_node;
  HOST_WIDE_INT n;
  location_t loc;

  check_no_duplicate_clause (list, OMP_CLAUSE_COLLAPSE, "collapse");

  loc = c_parser_peek_token (parser)->location;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      num = c_parser_expr_no_commas (parser, NULL).value;
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  if (num == error_mark_node)
    return list;
  mark_exp_read (num);
  num = c_fully_fold (num, false, NULL);
  if (!INTEGRAL_TYPE_P (TREE_TYPE (num))
      || !tree_fits_shwi_p (num)
      || (n = tree_to_shwi (num)) <= 0
      || (int) n != n)
    {
      error_at (loc,
		"collapse argument needs positive constant integer expression");
      return list;
    }
  c = build_omp_clause (loc, OMP_CLAUSE_COLLAPSE);
  OMP_CLAUSE_COLLAPSE_EXPR (c) = num;
  OMP_CLAUSE_CHAIN (c) = list;
  return c;
}

/* OpenMP 2.5:
   copyin ( variable-list ) */

static tree
c_parser_omp_clause_copyin (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_COPYIN, list);
}

/* OpenMP 2.5:
   copyprivate ( variable-list ) */

static tree
c_parser_omp_clause_copyprivate (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_COPYPRIVATE, list);
}

/* OpenMP 2.5:
   default ( shared | none ) */

static tree
c_parser_omp_clause_default (c_parser *parser, tree list)
{
  enum omp_clause_default_kind kind = OMP_CLAUSE_DEFAULT_UNSPECIFIED;
  location_t loc = c_parser_peek_token (parser)->location;
  tree c;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;
  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);

      switch (p[0])
	{
	case 'n':
	  if (strcmp ("none", p) != 0)
	    goto invalid_kind;
	  kind = OMP_CLAUSE_DEFAULT_NONE;
	  break;

	case 's':
	  if (strcmp ("shared", p) != 0)
	    goto invalid_kind;
	  kind = OMP_CLAUSE_DEFAULT_SHARED;
	  break;

	default:
	  goto invalid_kind;
	}

      c_parser_consume_token (parser);
    }
  else
    {
    invalid_kind:
      c_parser_error (parser, "expected %<none%> or %<shared%>");
    }
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");

  if (kind == OMP_CLAUSE_DEFAULT_UNSPECIFIED)
    return list;

  check_no_duplicate_clause (list, OMP_CLAUSE_DEFAULT, "default");
  c = build_omp_clause (loc, OMP_CLAUSE_DEFAULT);
  OMP_CLAUSE_CHAIN (c) = list;
  OMP_CLAUSE_DEFAULT_KIND (c) = kind;

  return c;
}

/* OpenMP 2.5:
   firstprivate ( variable-list ) */

static tree
c_parser_omp_clause_firstprivate (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_FIRSTPRIVATE, list);
}

/* OpenMP 3.1:
   final ( expression ) */

static tree
c_parser_omp_clause_final (c_parser *parser, tree list)
{
  location_t loc = c_parser_peek_token (parser)->location;
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      tree t = c_parser_paren_condition (parser);
      tree c;

      check_no_duplicate_clause (list, OMP_CLAUSE_FINAL, "final");

      c = build_omp_clause (loc, OMP_CLAUSE_FINAL);
      OMP_CLAUSE_FINAL_EXPR (c) = t;
      OMP_CLAUSE_CHAIN (c) = list;
      list = c;
    }
  else
    c_parser_error (parser, "expected %<(%>");

  return list;
}

/* OpenMP 2.5:
   if ( expression ) */

static tree
c_parser_omp_clause_if (c_parser *parser, tree list)
{
  location_t loc = c_parser_peek_token (parser)->location;
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      tree t = c_parser_paren_condition (parser);
      tree c;

      check_no_duplicate_clause (list, OMP_CLAUSE_IF, "if");

      c = build_omp_clause (loc, OMP_CLAUSE_IF);
      OMP_CLAUSE_IF_EXPR (c) = t;
      OMP_CLAUSE_CHAIN (c) = list;
      list = c;
    }
  else
    c_parser_error (parser, "expected %<(%>");

  return list;
}

/* OpenMP 2.5:
   lastprivate ( variable-list ) */

static tree
c_parser_omp_clause_lastprivate (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_LASTPRIVATE, list);
}

/* OpenMP 3.1:
   mergeable */

static tree
c_parser_omp_clause_mergeable (c_parser *parser ATTRIBUTE_UNUSED, tree list)
{
  tree c;

  /* FIXME: Should we allow duplicates?  */
  check_no_duplicate_clause (list, OMP_CLAUSE_MERGEABLE, "mergeable");

  c = build_omp_clause (c_parser_peek_token (parser)->location,
			OMP_CLAUSE_MERGEABLE);
  OMP_CLAUSE_CHAIN (c) = list;

  return c;
}

/* OpenMP 2.5:
   nowait */

static tree
c_parser_omp_clause_nowait (c_parser *parser ATTRIBUTE_UNUSED, tree list)
{
  tree c;
  location_t loc = c_parser_peek_token (parser)->location;

  check_no_duplicate_clause (list, OMP_CLAUSE_NOWAIT, "nowait");

  c = build_omp_clause (loc, OMP_CLAUSE_NOWAIT);
  OMP_CLAUSE_CHAIN (c) = list;
  return c;
}

/* OpenMP 2.5:
   num_threads ( expression ) */

static tree
c_parser_omp_clause_num_threads (c_parser *parser, tree list)
{
  location_t num_threads_loc = c_parser_peek_token (parser)->location;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      location_t expr_loc = c_parser_peek_token (parser)->location;
      tree c, t = c_parser_expression (parser).value;
      mark_exp_read (t);
      t = c_fully_fold (t, false, NULL);

      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");

      if (!INTEGRAL_TYPE_P (TREE_TYPE (t)))
	{
	  c_parser_error (parser, "expected integer expression");
	  return list;
	}

      /* Attempt to statically determine when the number isn't positive.  */
      c = fold_build2_loc (expr_loc, LE_EXPR, boolean_type_node, t,
		       build_int_cst (TREE_TYPE (t), 0));
      if (CAN_HAVE_LOCATION_P (c))
	SET_EXPR_LOCATION (c, expr_loc);
      if (c == boolean_true_node)
	{
	  warning_at (expr_loc, 0,
		      "%<num_threads%> value must be positive");
	  t = integer_one_node;
	}

      check_no_duplicate_clause (list, OMP_CLAUSE_NUM_THREADS, "num_threads");

      c = build_omp_clause (num_threads_loc, OMP_CLAUSE_NUM_THREADS);
      OMP_CLAUSE_NUM_THREADS_EXPR (c) = t;
      OMP_CLAUSE_CHAIN (c) = list;
      list = c;
    }

  return list;
}

/* OpenMP 2.5:
   ordered */

static tree
c_parser_omp_clause_ordered (c_parser *parser, tree list)
{
  tree c;

  check_no_duplicate_clause (list, OMP_CLAUSE_ORDERED, "ordered");

  c = build_omp_clause (c_parser_peek_token (parser)->location,
			OMP_CLAUSE_ORDERED);
  OMP_CLAUSE_CHAIN (c) = list;

  return c;
}

/* OpenMP 2.5:
   private ( variable-list ) */

static tree
c_parser_omp_clause_private (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_PRIVATE, list);
}

/* OpenMP 2.5:
   reduction ( reduction-operator : variable-list )

   reduction-operator:
     One of: + * - & ^ | && ||

   OpenMP 3.1:
   
   reduction-operator:
     One of: + * - & ^ | && || max min

   OpenMP 4.0:

   reduction-operator:
     One of: + * - & ^ | && ||
     identifier  */

static tree
c_parser_omp_clause_reduction (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      enum tree_code code = ERROR_MARK;
      tree reduc_id = NULL_TREE;

      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_PLUS:
	  code = PLUS_EXPR;
	  break;
	case CPP_MULT:
	  code = MULT_EXPR;
	  break;
	case CPP_MINUS:
	  code = MINUS_EXPR;
	  break;
	case CPP_AND:
	  code = BIT_AND_EXPR;
	  break;
	case CPP_XOR:
	  code = BIT_XOR_EXPR;
	  break;
	case CPP_OR:
	  code = BIT_IOR_EXPR;
	  break;
	case CPP_AND_AND:
	  code = TRUTH_ANDIF_EXPR;
	  break;
	case CPP_OR_OR:
	  code = TRUTH_ORIF_EXPR;
	  break;
        case CPP_NAME:
	  {
	    const char *p
	      = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
	    if (strcmp (p, "min") == 0)
	      {
		code = MIN_EXPR;
		break;
	      }
	    if (strcmp (p, "max") == 0)
	      {
		code = MAX_EXPR;
		break;
	      }
	    reduc_id = c_parser_peek_token (parser)->value;
	    break;
	  }
	default:
	  c_parser_error (parser,
			  "expected %<+%>, %<*%>, %<-%>, %<&%>, "
			  "%<^%>, %<|%>, %<&&%>, %<||%>, %<min%> or %<max%>");
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, 0);
	  return list;
	}
      c_parser_consume_token (parser);
      reduc_id = c_omp_reduction_id (code, reduc_id);
      if (c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	{
	  tree nl, c;

	  nl = c_parser_omp_variable_list (parser, clause_loc,
					   OMP_CLAUSE_REDUCTION, list);
	  for (c = nl; c != list; c = OMP_CLAUSE_CHAIN (c))
	    {
	      tree type = TREE_TYPE (OMP_CLAUSE_DECL (c));
	      OMP_CLAUSE_REDUCTION_CODE (c) = code;
	      if (code == ERROR_MARK
		  || !(INTEGRAL_TYPE_P (type)
		       || TREE_CODE (type) == REAL_TYPE
		       || TREE_CODE (type) == COMPLEX_TYPE))
		OMP_CLAUSE_REDUCTION_PLACEHOLDER (c)
		  = c_omp_reduction_lookup (reduc_id,
					    TYPE_MAIN_VARIANT (type));
	    }

	  list = nl;
	}
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  return list;
}

/* OpenMP 2.5:
   schedule ( schedule-kind )
   schedule ( schedule-kind , expression )

   schedule-kind:
     static | dynamic | guided | runtime | auto
*/

static tree
c_parser_omp_clause_schedule (c_parser *parser, tree list)
{
  tree c, t;
  location_t loc = c_parser_peek_token (parser)->location;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  c = build_omp_clause (loc, OMP_CLAUSE_SCHEDULE);

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      tree kind = c_parser_peek_token (parser)->value;
      const char *p = IDENTIFIER_POINTER (kind);

      switch (p[0])
	{
	case 'd':
	  if (strcmp ("dynamic", p) != 0)
	    goto invalid_kind;
	  OMP_CLAUSE_SCHEDULE_KIND (c) = OMP_CLAUSE_SCHEDULE_DYNAMIC;
	  break;

        case 'g':
	  if (strcmp ("guided", p) != 0)
	    goto invalid_kind;
	  OMP_CLAUSE_SCHEDULE_KIND (c) = OMP_CLAUSE_SCHEDULE_GUIDED;
	  break;

	case 'r':
	  if (strcmp ("runtime", p) != 0)
	    goto invalid_kind;
	  OMP_CLAUSE_SCHEDULE_KIND (c) = OMP_CLAUSE_SCHEDULE_RUNTIME;
	  break;

	default:
	  goto invalid_kind;
	}
    }
  else if (c_parser_next_token_is_keyword (parser, RID_STATIC))
    OMP_CLAUSE_SCHEDULE_KIND (c) = OMP_CLAUSE_SCHEDULE_STATIC;
  else if (c_parser_next_token_is_keyword (parser, RID_AUTO))
    OMP_CLAUSE_SCHEDULE_KIND (c) = OMP_CLAUSE_SCHEDULE_AUTO;
  else
    goto invalid_kind;

  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_COMMA))
    {
      location_t here;
      c_parser_consume_token (parser);

      here = c_parser_peek_token (parser)->location;
      t = c_parser_expr_no_commas (parser, NULL).value;
      mark_exp_read (t);
      t = c_fully_fold (t, false, NULL);

      if (OMP_CLAUSE_SCHEDULE_KIND (c) == OMP_CLAUSE_SCHEDULE_RUNTIME)
	error_at (here, "schedule %<runtime%> does not take "
		  "a %<chunk_size%> parameter");
      else if (OMP_CLAUSE_SCHEDULE_KIND (c) == OMP_CLAUSE_SCHEDULE_AUTO)
	error_at (here,
		  "schedule %<auto%> does not take "
		  "a %<chunk_size%> parameter");
      else if (TREE_CODE (TREE_TYPE (t)) == INTEGER_TYPE)
	OMP_CLAUSE_SCHEDULE_CHUNK_EXPR (c) = t;
      else
	c_parser_error (parser, "expected integer expression");

      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  else
    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
			       "expected %<,%> or %<)%>");

  check_no_duplicate_clause (list, OMP_CLAUSE_SCHEDULE, "schedule");
  OMP_CLAUSE_CHAIN (c) = list;
  return c;

 invalid_kind:
  c_parser_error (parser, "invalid schedule kind");
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, 0);
  return list;
}

/* OpenMP 2.5:
   shared ( variable-list ) */

static tree
c_parser_omp_clause_shared (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_SHARED, list);
}

/* OpenMP 3.0:
   untied */

static tree
c_parser_omp_clause_untied (c_parser *parser ATTRIBUTE_UNUSED, tree list)
{
  tree c;

  /* FIXME: Should we allow duplicates?  */
  check_no_duplicate_clause (list, OMP_CLAUSE_UNTIED, "untied");

  c = build_omp_clause (c_parser_peek_token (parser)->location,
			OMP_CLAUSE_UNTIED);
  OMP_CLAUSE_CHAIN (c) = list;

  return c;
}

/* OpenMP 4.0:
   inbranch
   notinbranch */

static tree
c_parser_omp_clause_branch (c_parser *parser ATTRIBUTE_UNUSED,
			    enum omp_clause_code code, tree list)
{
  check_no_duplicate_clause (list, code, omp_clause_code_name[code]);

  tree c = build_omp_clause (c_parser_peek_token (parser)->location, code);
  OMP_CLAUSE_CHAIN (c) = list;

  return c;
}

/* OpenMP 4.0:
   parallel
   for
   sections
   taskgroup */

static tree
c_parser_omp_clause_cancelkind (c_parser *parser ATTRIBUTE_UNUSED,
				enum omp_clause_code code, tree list)
{
  tree c = build_omp_clause (c_parser_peek_token (parser)->location, code);
  OMP_CLAUSE_CHAIN (c) = list;

  return c;
}

/* OpenMP 4.0:
   num_teams ( expression ) */

static tree
c_parser_omp_clause_num_teams (c_parser *parser, tree list)
{
  location_t num_teams_loc = c_parser_peek_token (parser)->location;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      location_t expr_loc = c_parser_peek_token (parser)->location;
      tree c, t = c_parser_expression (parser).value;
      mark_exp_read (t);
      t = c_fully_fold (t, false, NULL);

      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");

      if (!INTEGRAL_TYPE_P (TREE_TYPE (t)))
	{
	  c_parser_error (parser, "expected integer expression");
	  return list;
	}

      /* Attempt to statically determine when the number isn't positive.  */
      c = fold_build2_loc (expr_loc, LE_EXPR, boolean_type_node, t,
			   build_int_cst (TREE_TYPE (t), 0));
      if (CAN_HAVE_LOCATION_P (c))
	SET_EXPR_LOCATION (c, expr_loc);
      if (c == boolean_true_node)
	{
	  warning_at (expr_loc, 0, "%<num_teams%> value must be positive");
	  t = integer_one_node;
	}

      check_no_duplicate_clause (list, OMP_CLAUSE_NUM_TEAMS, "num_teams");

      c = build_omp_clause (num_teams_loc, OMP_CLAUSE_NUM_TEAMS);
      OMP_CLAUSE_NUM_TEAMS_EXPR (c) = t;
      OMP_CLAUSE_CHAIN (c) = list;
      list = c;
    }

  return list;
}

/* OpenMP 4.0:
   thread_limit ( expression ) */

static tree
c_parser_omp_clause_thread_limit (c_parser *parser, tree list)
{
  location_t num_thread_limit_loc = c_parser_peek_token (parser)->location;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      location_t expr_loc = c_parser_peek_token (parser)->location;
      tree c, t = c_parser_expression (parser).value;
      mark_exp_read (t);
      t = c_fully_fold (t, false, NULL);

      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");

      if (!INTEGRAL_TYPE_P (TREE_TYPE (t)))
	{
	  c_parser_error (parser, "expected integer expression");
	  return list;
	}

      /* Attempt to statically determine when the number isn't positive.  */
      c = fold_build2_loc (expr_loc, LE_EXPR, boolean_type_node, t,
			   build_int_cst (TREE_TYPE (t), 0));
      if (CAN_HAVE_LOCATION_P (c))
	SET_EXPR_LOCATION (c, expr_loc);
      if (c == boolean_true_node)
	{
	  warning_at (expr_loc, 0, "%<thread_limit%> value must be positive");
	  t = integer_one_node;
	}

      check_no_duplicate_clause (list, OMP_CLAUSE_THREAD_LIMIT,
				 "thread_limit");

      c = build_omp_clause (num_thread_limit_loc, OMP_CLAUSE_THREAD_LIMIT);
      OMP_CLAUSE_THREAD_LIMIT_EXPR (c) = t;
      OMP_CLAUSE_CHAIN (c) = list;
      list = c;
    }

  return list;
}

/* OpenMP 4.0:
   aligned ( variable-list )
   aligned ( variable-list : constant-expression ) */

static tree
c_parser_omp_clause_aligned (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  tree nl, c;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  nl = c_parser_omp_variable_list (parser, clause_loc,
				   OMP_CLAUSE_ALIGNED, list);

  if (c_parser_next_token_is (parser, CPP_COLON))
    {
      c_parser_consume_token (parser);
      tree alignment = c_parser_expr_no_commas (parser, NULL).value;
      mark_exp_read (alignment);
      alignment = c_fully_fold (alignment, false, NULL);
      if (!INTEGRAL_TYPE_P (TREE_TYPE (alignment))
	  && TREE_CODE (alignment) != INTEGER_CST
	  && tree_int_cst_sgn (alignment) != 1)
	{
	  error_at (clause_loc, "%<aligned%> clause alignment expression must "
				"be positive constant integer expression");
	  alignment = NULL_TREE;
	}

      for (c = nl; c != list; c = OMP_CLAUSE_CHAIN (c))
	OMP_CLAUSE_ALIGNED_ALIGNMENT (c) = alignment;
    }

  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return nl;
}

/* OpenMP 4.0:
   linear ( variable-list )
   linear ( variable-list : expression ) */

static tree
c_parser_omp_clause_linear (c_parser *parser, tree list, bool is_cilk_simd_fn)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  tree nl, c, step;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  nl = c_parser_omp_variable_list (parser, clause_loc,
				   OMP_CLAUSE_LINEAR, list);

  if (c_parser_next_token_is (parser, CPP_COLON))
    {
      c_parser_consume_token (parser);
      step = c_parser_expression (parser).value;
      mark_exp_read (step);
      step = c_fully_fold (step, false, NULL);
      if (is_cilk_simd_fn && TREE_CODE (step) == PARM_DECL)
	{
	  sorry ("using parameters for %<linear%> step is not supported yet");
	  step = integer_one_node;
	}
      if (!INTEGRAL_TYPE_P (TREE_TYPE (step)))
	{
	  error_at (clause_loc, "%<linear%> clause step expression must "
				"be integral");
	  step = integer_one_node;
	}

    }
  else
    step = integer_one_node;

  for (c = nl; c != list; c = OMP_CLAUSE_CHAIN (c))
    {
      OMP_CLAUSE_LINEAR_STEP (c) = step;
    }

  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return nl;
}

/* OpenMP 4.0:
   safelen ( constant-expression ) */

static tree
c_parser_omp_clause_safelen (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  tree c, t;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  t = c_parser_expr_no_commas (parser, NULL).value;
  mark_exp_read (t);
  t = c_fully_fold (t, false, NULL);
  if (!INTEGRAL_TYPE_P (TREE_TYPE (t))
      && TREE_CODE (t) != INTEGER_CST
      && tree_int_cst_sgn (t) != 1)
    {
      error_at (clause_loc, "%<safelen%> clause expression must "
			    "be positive constant integer expression");
      t = NULL_TREE;
    }

  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  if (t == NULL_TREE || t == error_mark_node)
    return list;

  check_no_duplicate_clause (list, OMP_CLAUSE_SAFELEN, "safelen");

  c = build_omp_clause (clause_loc, OMP_CLAUSE_SAFELEN);
  OMP_CLAUSE_SAFELEN_EXPR (c) = t;
  OMP_CLAUSE_CHAIN (c) = list;
  return c;
}

/* OpenMP 4.0:
   simdlen ( constant-expression ) */

static tree
c_parser_omp_clause_simdlen (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  tree c, t;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  t = c_parser_expr_no_commas (parser, NULL).value;
  mark_exp_read (t);
  t = c_fully_fold (t, false, NULL);
  if (!INTEGRAL_TYPE_P (TREE_TYPE (t))
      && TREE_CODE (t) != INTEGER_CST
      && tree_int_cst_sgn (t) != 1)
    {
      error_at (clause_loc, "%<simdlen%> clause expression must "
			    "be positive constant integer expression");
      t = NULL_TREE;
    }

  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  if (t == NULL_TREE || t == error_mark_node)
    return list;

  check_no_duplicate_clause (list, OMP_CLAUSE_SIMDLEN, "simdlen");

  c = build_omp_clause (clause_loc, OMP_CLAUSE_SIMDLEN);
  OMP_CLAUSE_SIMDLEN_EXPR (c) = t;
  OMP_CLAUSE_CHAIN (c) = list;
  return c;
}

/* OpenMP 4.0:
   depend ( depend-kind: variable-list )

   depend-kind:
     in | out | inout  */

static tree
c_parser_omp_clause_depend (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  enum omp_clause_depend_kind kind = OMP_CLAUSE_DEPEND_INOUT;
  tree nl, c;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp ("in", p) == 0)
	kind = OMP_CLAUSE_DEPEND_IN;
      else if (strcmp ("inout", p) == 0)
	kind = OMP_CLAUSE_DEPEND_INOUT;
      else if (strcmp ("out", p) == 0)
	kind = OMP_CLAUSE_DEPEND_OUT;
      else
	goto invalid_kind;
    }
  else
    goto invalid_kind;

  c_parser_consume_token (parser);
  if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
    goto resync_fail;

  nl = c_parser_omp_variable_list (parser, clause_loc,
				   OMP_CLAUSE_DEPEND, list);

  for (c = nl; c != list; c = OMP_CLAUSE_CHAIN (c))
    OMP_CLAUSE_DEPEND_KIND (c) = kind;

  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return nl;

 invalid_kind:
  c_parser_error (parser, "invalid depend kind");
 resync_fail:
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return list;
}

/* OpenMP 4.0:
   map ( map-kind: variable-list )
   map ( variable-list )

   map-kind:
     alloc | to | from | tofrom  */

static tree
c_parser_omp_clause_map (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  enum omp_clause_map_kind kind = OMP_CLAUSE_MAP_TOFROM;
  tree nl, c;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  if (c_parser_next_token_is (parser, CPP_NAME)
      && c_parser_peek_2nd_token (parser)->type == CPP_COLON)
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp ("alloc", p) == 0)
	kind = OMP_CLAUSE_MAP_ALLOC;
      else if (strcmp ("to", p) == 0)
	kind = OMP_CLAUSE_MAP_TO;
      else if (strcmp ("from", p) == 0)
	kind = OMP_CLAUSE_MAP_FROM;
      else if (strcmp ("tofrom", p) == 0)
	kind = OMP_CLAUSE_MAP_TOFROM;
      else
	{
	  c_parser_error (parser, "invalid map kind");
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return list;
	}
      c_parser_consume_token (parser);
      c_parser_consume_token (parser);
    }

  nl = c_parser_omp_variable_list (parser, clause_loc, OMP_CLAUSE_MAP, list);

  for (c = nl; c != list; c = OMP_CLAUSE_CHAIN (c))
    OMP_CLAUSE_MAP_KIND (c) = kind;

  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return nl;
}

/* OpenMP 4.0:
   device ( expression ) */

static tree
c_parser_omp_clause_device (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      tree c, t = c_parser_expr_no_commas (parser, NULL).value;
      mark_exp_read (t);
      t = c_fully_fold (t, false, NULL);

      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");

      if (!INTEGRAL_TYPE_P (TREE_TYPE (t)))
	{
	  c_parser_error (parser, "expected integer expression");
	  return list;
	}

      check_no_duplicate_clause (list, OMP_CLAUSE_DEVICE, "device");

      c = build_omp_clause (clause_loc, OMP_CLAUSE_DEVICE);
      OMP_CLAUSE_DEVICE_ID (c) = t;
      OMP_CLAUSE_CHAIN (c) = list;
      list = c;
    }

  return list;
}

/* OpenMP 4.0:
   dist_schedule ( static )
   dist_schedule ( static , expression ) */

static tree
c_parser_omp_clause_dist_schedule (c_parser *parser, tree list)
{
  tree c, t = NULL_TREE;
  location_t loc = c_parser_peek_token (parser)->location;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  if (!c_parser_next_token_is_keyword (parser, RID_STATIC))
    {
      c_parser_error (parser, "invalid dist_schedule kind");
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				 "expected %<)%>");
      return list;
    }

  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_COMMA))
    {
      c_parser_consume_token (parser);

      t = c_parser_expr_no_commas (parser, NULL).value;
      mark_exp_read (t);
      t = c_fully_fold (t, false, NULL);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  else
    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
			       "expected %<,%> or %<)%>");

  check_no_duplicate_clause (list, OMP_CLAUSE_SCHEDULE, "schedule");
  if (t == error_mark_node)
    return list;

  c = build_omp_clause (loc, OMP_CLAUSE_DIST_SCHEDULE);
  OMP_CLAUSE_DIST_SCHEDULE_CHUNK_EXPR (c) = t;
  OMP_CLAUSE_CHAIN (c) = list;
  return c;
}

/* OpenMP 4.0:
   proc_bind ( proc-bind-kind )

   proc-bind-kind:
     master | close | spread  */

static tree
c_parser_omp_clause_proc_bind (c_parser *parser, tree list)
{
  location_t clause_loc = c_parser_peek_token (parser)->location;
  enum omp_clause_proc_bind_kind kind;
  tree c;

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return list;

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp ("master", p) == 0)
	kind = OMP_CLAUSE_PROC_BIND_MASTER;
      else if (strcmp ("close", p) == 0)
	kind = OMP_CLAUSE_PROC_BIND_CLOSE;
      else if (strcmp ("spread", p) == 0)
	kind = OMP_CLAUSE_PROC_BIND_SPREAD;
      else
	goto invalid_kind;
    }
  else
    goto invalid_kind;

  c_parser_consume_token (parser);
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  c = build_omp_clause (clause_loc, OMP_CLAUSE_PROC_BIND);
  OMP_CLAUSE_PROC_BIND_KIND (c) = kind;
  OMP_CLAUSE_CHAIN (c) = list;
  return c;

 invalid_kind:
  c_parser_error (parser, "invalid proc_bind kind");
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
  return list;
}

/* OpenMP 4.0:
   to ( variable-list ) */

static tree
c_parser_omp_clause_to (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_TO, list);
}

/* OpenMP 4.0:
   from ( variable-list ) */

static tree
c_parser_omp_clause_from (c_parser *parser, tree list)
{
  return c_parser_omp_var_list_parens (parser, OMP_CLAUSE_FROM, list);
}

/* OpenMP 4.0:
   uniform ( variable-list ) */

static tree
c_parser_omp_clause_uniform (c_parser *parser, tree list)
{
  /* The clauses location.  */
  location_t loc = c_parser_peek_token (parser)->location;

  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      list = c_parser_omp_variable_list (parser, loc, OMP_CLAUSE_UNIFORM,
					 list);
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");
    }
  return list;
}

/* Parse all OpenMP clauses.  The set clauses allowed by the directive
   is a bitmask in MASK.  Return the list of clauses found; the result
   of clause default goes in *pdefault.  */

static tree
c_parser_omp_all_clauses (c_parser *parser, omp_clause_mask mask,
			  const char *where, bool finish_p = true)
{
  tree clauses = NULL;
  bool first = true, cilk_simd_fn = false;

  while (c_parser_next_token_is_not (parser, CPP_PRAGMA_EOL))
    {
      location_t here;
      pragma_omp_clause c_kind;
      const char *c_name;
      tree prev = clauses;

      if (!first && c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);

      here = c_parser_peek_token (parser)->location;
      c_kind = c_parser_omp_clause_name (parser);

      switch (c_kind)
	{
	case PRAGMA_OMP_CLAUSE_COLLAPSE:
	  clauses = c_parser_omp_clause_collapse (parser, clauses);
	  c_name = "collapse";
	  break;
	case PRAGMA_OMP_CLAUSE_COPYIN:
	  clauses = c_parser_omp_clause_copyin (parser, clauses);
	  c_name = "copyin";
	  break;
	case PRAGMA_OMP_CLAUSE_COPYPRIVATE:
	  clauses = c_parser_omp_clause_copyprivate (parser, clauses);
	  c_name = "copyprivate";
	  break;
	case PRAGMA_OMP_CLAUSE_DEFAULT:
	  clauses = c_parser_omp_clause_default (parser, clauses);
	  c_name = "default";
	  break;
	case PRAGMA_OMP_CLAUSE_FIRSTPRIVATE:
	  clauses = c_parser_omp_clause_firstprivate (parser, clauses);
	  c_name = "firstprivate";
	  break;
	case PRAGMA_OMP_CLAUSE_FINAL:
	  clauses = c_parser_omp_clause_final (parser, clauses);
	  c_name = "final";
	  break;
	case PRAGMA_OMP_CLAUSE_IF:
	  clauses = c_parser_omp_clause_if (parser, clauses);
	  c_name = "if";
	  break;
	case PRAGMA_OMP_CLAUSE_LASTPRIVATE:
	  clauses = c_parser_omp_clause_lastprivate (parser, clauses);
	  c_name = "lastprivate";
	  break;
	case PRAGMA_OMP_CLAUSE_MERGEABLE:
	  clauses = c_parser_omp_clause_mergeable (parser, clauses);
	  c_name = "mergeable";
	  break;
	case PRAGMA_OMP_CLAUSE_NOWAIT:
	  clauses = c_parser_omp_clause_nowait (parser, clauses);
	  c_name = "nowait";
	  break;
	case PRAGMA_OMP_CLAUSE_NUM_THREADS:
	  clauses = c_parser_omp_clause_num_threads (parser, clauses);
	  c_name = "num_threads";
	  break;
	case PRAGMA_OMP_CLAUSE_ORDERED:
	  clauses = c_parser_omp_clause_ordered (parser, clauses);
	  c_name = "ordered";
	  break;
	case PRAGMA_OMP_CLAUSE_PRIVATE:
	  clauses = c_parser_omp_clause_private (parser, clauses);
	  c_name = "private";
	  break;
	case PRAGMA_OMP_CLAUSE_REDUCTION:
	  clauses = c_parser_omp_clause_reduction (parser, clauses);
	  c_name = "reduction";
	  break;
	case PRAGMA_OMP_CLAUSE_SCHEDULE:
	  clauses = c_parser_omp_clause_schedule (parser, clauses);
	  c_name = "schedule";
	  break;
	case PRAGMA_OMP_CLAUSE_SHARED:
	  clauses = c_parser_omp_clause_shared (parser, clauses);
	  c_name = "shared";
	  break;
	case PRAGMA_OMP_CLAUSE_UNTIED:
	  clauses = c_parser_omp_clause_untied (parser, clauses);
	  c_name = "untied";
	  break;
	case PRAGMA_OMP_CLAUSE_INBRANCH:
	case PRAGMA_CILK_CLAUSE_MASK:
	  clauses = c_parser_omp_clause_branch (parser, OMP_CLAUSE_INBRANCH,
						clauses);
	  c_name = "inbranch";
	  break;
	case PRAGMA_OMP_CLAUSE_NOTINBRANCH:
	case PRAGMA_CILK_CLAUSE_NOMASK:
	  clauses = c_parser_omp_clause_branch (parser, OMP_CLAUSE_NOTINBRANCH,
						clauses);
	  c_name = "notinbranch";
	  break;
	case PRAGMA_OMP_CLAUSE_PARALLEL:
	  clauses
	    = c_parser_omp_clause_cancelkind (parser, OMP_CLAUSE_PARALLEL,
					      clauses);
	  c_name = "parallel";
	  if (!first)
	    {
	     clause_not_first:
	      error_at (here, "%qs must be the first clause of %qs",
			c_name, where);
	      clauses = prev;
	    }
	  break;
	case PRAGMA_OMP_CLAUSE_FOR:
	  clauses
	    = c_parser_omp_clause_cancelkind (parser, OMP_CLAUSE_FOR,
					      clauses);
	  c_name = "for";
	  if (!first)
	    goto clause_not_first;
	  break;
	case PRAGMA_OMP_CLAUSE_SECTIONS:
	  clauses
	    = c_parser_omp_clause_cancelkind (parser, OMP_CLAUSE_SECTIONS,
					      clauses);
	  c_name = "sections";
	  if (!first)
	    goto clause_not_first;
	  break;
	case PRAGMA_OMP_CLAUSE_TASKGROUP:
	  clauses
	    = c_parser_omp_clause_cancelkind (parser, OMP_CLAUSE_TASKGROUP,
					      clauses);
	  c_name = "taskgroup";
	  if (!first)
	    goto clause_not_first;
	  break;
	case PRAGMA_OMP_CLAUSE_TO:
	  clauses = c_parser_omp_clause_to (parser, clauses);
	  c_name = "to";
	  break;
	case PRAGMA_OMP_CLAUSE_FROM:
	  clauses = c_parser_omp_clause_from (parser, clauses);
	  c_name = "from";
	  break;
	case PRAGMA_OMP_CLAUSE_UNIFORM:
	  clauses = c_parser_omp_clause_uniform (parser, clauses);
	  c_name = "uniform";
	  break;
	case PRAGMA_OMP_CLAUSE_NUM_TEAMS:
	  clauses = c_parser_omp_clause_num_teams (parser, clauses);
	  c_name = "num_teams";
	  break;
	case PRAGMA_OMP_CLAUSE_THREAD_LIMIT:
	  clauses = c_parser_omp_clause_thread_limit (parser, clauses);
	  c_name = "thread_limit";
	  break;
	case PRAGMA_OMP_CLAUSE_ALIGNED:
	  clauses = c_parser_omp_clause_aligned (parser, clauses);
	  c_name = "aligned";
	  break;
	case PRAGMA_OMP_CLAUSE_LINEAR: 
	  if (((mask >> PRAGMA_CILK_CLAUSE_VECTORLENGTH) & 1) != 0)
	   cilk_simd_fn = true; 
	  clauses = c_parser_omp_clause_linear (parser, clauses, cilk_simd_fn); 
	  c_name = "linear";
	  break;
	case PRAGMA_OMP_CLAUSE_DEPEND:
	  clauses = c_parser_omp_clause_depend (parser, clauses);
	  c_name = "depend";
	  break;
	case PRAGMA_OMP_CLAUSE_MAP:
	  clauses = c_parser_omp_clause_map (parser, clauses);
	  c_name = "map";
	  break;
	case PRAGMA_OMP_CLAUSE_DEVICE:
	  clauses = c_parser_omp_clause_device (parser, clauses);
	  c_name = "device";
	  break;
	case PRAGMA_OMP_CLAUSE_DIST_SCHEDULE:
	  clauses = c_parser_omp_clause_dist_schedule (parser, clauses);
	  c_name = "dist_schedule";
	  break;
	case PRAGMA_OMP_CLAUSE_PROC_BIND:
	  clauses = c_parser_omp_clause_proc_bind (parser, clauses);
	  c_name = "proc_bind";
	  break;
	case PRAGMA_OMP_CLAUSE_SAFELEN:
	  clauses = c_parser_omp_clause_safelen (parser, clauses);
	  c_name = "safelen";
	  break;
	case PRAGMA_CILK_CLAUSE_VECTORLENGTH:
	  clauses = c_parser_cilk_clause_vectorlength (parser, clauses, true);
	  c_name = "simdlen";
	  break;
	case PRAGMA_OMP_CLAUSE_SIMDLEN:
	  clauses = c_parser_omp_clause_simdlen (parser, clauses);
	  c_name = "simdlen";
	  break;
	default:
	  c_parser_error (parser, "expected %<#pragma omp%> clause");
	  goto saw_error;
	}

      first = false;

      if (((mask >> c_kind) & 1) == 0 && !parser->error)
	{
	  /* Remove the invalid clause(s) from the list to avoid
	     confusing the rest of the compiler.  */
	  clauses = prev;
	  error_at (here, "%qs is not valid for %qs", c_name, where);
	}
    }

 saw_error:
  c_parser_skip_to_pragma_eol (parser);

  if (finish_p)
    return c_finish_omp_clauses (clauses);

  return clauses;
}

/* OpenMP 2.5:
   structured-block:
     statement

   In practice, we're also interested in adding the statement to an
   outer node.  So it is convenient if we work around the fact that
   c_parser_statement calls add_stmt.  */

static tree
c_parser_omp_structured_block (c_parser *parser)
{
  tree stmt = push_stmt_list ();
  c_parser_statement (parser);
  return pop_stmt_list (stmt);
}

/* OpenMP 2.5:
   # pragma omp atomic new-line
     expression-stmt

   expression-stmt:
     x binop= expr | x++ | ++x | x-- | --x
   binop:
     +, *, -, /, &, ^, |, <<, >>

  where x is an lvalue expression with scalar type.

   OpenMP 3.1:
   # pragma omp atomic new-line
     update-stmt

   # pragma omp atomic read new-line
     read-stmt

   # pragma omp atomic write new-line
     write-stmt

   # pragma omp atomic update new-line
     update-stmt

   # pragma omp atomic capture new-line
     capture-stmt

   # pragma omp atomic capture new-line
     capture-block

   read-stmt:
     v = x
   write-stmt:
     x = expr
   update-stmt:
     expression-stmt | x = x binop expr
   capture-stmt:
     v = expression-stmt
   capture-block:
     { v = x; update-stmt; } | { update-stmt; v = x; }

   OpenMP 4.0:
   update-stmt:
     expression-stmt | x = x binop expr | x = expr binop x
   capture-stmt:
     v = update-stmt
   capture-block:
     { v = x; update-stmt; } | { update-stmt; v = x; } | { v = x; x = expr; }

  where x and v are lvalue expressions with scalar type.

  LOC is the location of the #pragma token.  */

static void
c_parser_omp_atomic (location_t loc, c_parser *parser)
{
  tree lhs = NULL_TREE, rhs = NULL_TREE, v = NULL_TREE;
  tree lhs1 = NULL_TREE, rhs1 = NULL_TREE;
  tree stmt, orig_lhs, unfolded_lhs = NULL_TREE, unfolded_lhs1 = NULL_TREE;
  enum tree_code code = OMP_ATOMIC, opcode = NOP_EXPR;
  struct c_expr expr;
  location_t eloc;
  bool structured_block = false;
  bool swapped = false;
  bool seq_cst = false;

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (!strcmp (p, "seq_cst"))
	{
	  seq_cst = true;
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_COMMA)
	      && c_parser_peek_2nd_token (parser)->type == CPP_NAME)
	    c_parser_consume_token (parser);
	}
    }
  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);

      if (!strcmp (p, "read"))
	code = OMP_ATOMIC_READ;
      else if (!strcmp (p, "write"))
	code = NOP_EXPR;
      else if (!strcmp (p, "update"))
	code = OMP_ATOMIC;
      else if (!strcmp (p, "capture"))
	code = OMP_ATOMIC_CAPTURE_NEW;
      else
	p = NULL;
      if (p)
	c_parser_consume_token (parser);
    }
  if (!seq_cst)
    {
      if (c_parser_next_token_is (parser, CPP_COMMA)
	  && c_parser_peek_2nd_token (parser)->type == CPP_NAME)
	c_parser_consume_token (parser);

      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  const char *p
	    = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
	  if (!strcmp (p, "seq_cst"))
	    {
	      seq_cst = true;
	      c_parser_consume_token (parser);
	    }
	}
    }
  c_parser_skip_to_pragma_eol (parser);

  switch (code)
    {
    case OMP_ATOMIC_READ:
    case NOP_EXPR: /* atomic write */
      v = c_parser_unary_expression (parser).value;
      v = c_fully_fold (v, false, NULL);
      if (v == error_mark_node)
	goto saw_error;
      loc = c_parser_peek_token (parser)->location;
      if (!c_parser_require (parser, CPP_EQ, "expected %<=%>"))
	goto saw_error;
      if (code == NOP_EXPR)
	lhs = c_parser_expression (parser).value;
      else
	lhs = c_parser_unary_expression (parser).value;
      lhs = c_fully_fold (lhs, false, NULL);
      if (lhs == error_mark_node)
	goto saw_error;
      if (code == NOP_EXPR)
	{
	  /* atomic write is represented by OMP_ATOMIC with NOP_EXPR
	     opcode.  */
	  code = OMP_ATOMIC;
	  rhs = lhs;
	  lhs = v;
	  v = NULL_TREE;
	}
      goto done;
    case OMP_ATOMIC_CAPTURE_NEW:
      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	{
	  c_parser_consume_token (parser);
	  structured_block = true;
	}
      else
	{
	  v = c_parser_unary_expression (parser).value;
	  v = c_fully_fold (v, false, NULL);
	  if (v == error_mark_node)
	    goto saw_error;
	  if (!c_parser_require (parser, CPP_EQ, "expected %<=%>"))
	    goto saw_error;
	}
      break;
    default:
      break;
    }

  /* For structured_block case we don't know yet whether
     old or new x should be captured.  */
restart:
  eloc = c_parser_peek_token (parser)->location;
  expr = c_parser_unary_expression (parser);
  lhs = expr.value;
  expr = default_function_array_conversion (eloc, expr);
  unfolded_lhs = expr.value;
  lhs = c_fully_fold (lhs, false, NULL);
  orig_lhs = lhs;
  switch (TREE_CODE (lhs))
    {
    case ERROR_MARK:
    saw_error:
      c_parser_skip_to_end_of_block_or_statement (parser);
      if (structured_block)
	{
	  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    c_parser_consume_token (parser);
	  else if (code == OMP_ATOMIC_CAPTURE_NEW)
	    {
	      c_parser_skip_to_end_of_block_or_statement (parser);
	      if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
		c_parser_consume_token (parser);
	    }
	}
      return;

    case POSTINCREMENT_EXPR:
      if (code == OMP_ATOMIC_CAPTURE_NEW && !structured_block)
	code = OMP_ATOMIC_CAPTURE_OLD;
      /* FALLTHROUGH */
    case PREINCREMENT_EXPR:
      lhs = TREE_OPERAND (lhs, 0);
      unfolded_lhs = NULL_TREE;
      opcode = PLUS_EXPR;
      rhs = integer_one_node;
      break;

    case POSTDECREMENT_EXPR:
      if (code == OMP_ATOMIC_CAPTURE_NEW && !structured_block)
	code = OMP_ATOMIC_CAPTURE_OLD;
      /* FALLTHROUGH */
    case PREDECREMENT_EXPR:
      lhs = TREE_OPERAND (lhs, 0);
      unfolded_lhs = NULL_TREE;
      opcode = MINUS_EXPR;
      rhs = integer_one_node;
      break;

    case COMPOUND_EXPR:
      if (TREE_CODE (TREE_OPERAND (lhs, 0)) == SAVE_EXPR
	  && TREE_CODE (TREE_OPERAND (lhs, 1)) == COMPOUND_EXPR
	  && TREE_CODE (TREE_OPERAND (TREE_OPERAND (lhs, 1), 0)) == MODIFY_EXPR
	  && TREE_OPERAND (TREE_OPERAND (lhs, 1), 1) == TREE_OPERAND (lhs, 0)
	  && TREE_CODE (TREE_TYPE (TREE_OPERAND (TREE_OPERAND
					      (TREE_OPERAND (lhs, 1), 0), 0)))
	     == BOOLEAN_TYPE)
	/* Undo effects of boolean_increment for post {in,de}crement.  */
	lhs = TREE_OPERAND (TREE_OPERAND (lhs, 1), 0);
      /* FALLTHRU */
    case MODIFY_EXPR:
      if (TREE_CODE (lhs) == MODIFY_EXPR
	  && TREE_CODE (TREE_TYPE (TREE_OPERAND (lhs, 0))) == BOOLEAN_TYPE)
	{
	  /* Undo effects of boolean_increment.  */
	  if (integer_onep (TREE_OPERAND (lhs, 1)))
	    {
	      /* This is pre or post increment.  */
	      rhs = TREE_OPERAND (lhs, 1);
	      lhs = TREE_OPERAND (lhs, 0);
	      unfolded_lhs = NULL_TREE;
	      opcode = NOP_EXPR;
	      if (code == OMP_ATOMIC_CAPTURE_NEW
		  && !structured_block
		  && TREE_CODE (orig_lhs) == COMPOUND_EXPR)
		code = OMP_ATOMIC_CAPTURE_OLD;
	      break;
	    }
	  if (TREE_CODE (TREE_OPERAND (lhs, 1)) == TRUTH_NOT_EXPR
	      && TREE_OPERAND (lhs, 0)
		 == TREE_OPERAND (TREE_OPERAND (lhs, 1), 0))
	    {
	      /* This is pre or post decrement.  */
	      rhs = TREE_OPERAND (lhs, 1);
	      lhs = TREE_OPERAND (lhs, 0);
	      unfolded_lhs = NULL_TREE;
	      opcode = NOP_EXPR;
	      if (code == OMP_ATOMIC_CAPTURE_NEW
		  && !structured_block
		  && TREE_CODE (orig_lhs) == COMPOUND_EXPR)
		code = OMP_ATOMIC_CAPTURE_OLD;
	      break;
	    }
	}
      /* FALLTHRU */
    default:
      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_MULT_EQ:
	  opcode = MULT_EXPR;
	  break;
	case CPP_DIV_EQ:
	  opcode = TRUNC_DIV_EXPR;
	  break;
	case CPP_PLUS_EQ:
	  opcode = PLUS_EXPR;
	  break;
	case CPP_MINUS_EQ:
	  opcode = MINUS_EXPR;
	  break;
	case CPP_LSHIFT_EQ:
	  opcode = LSHIFT_EXPR;
	  break;
	case CPP_RSHIFT_EQ:
	  opcode = RSHIFT_EXPR;
	  break;
	case CPP_AND_EQ:
	  opcode = BIT_AND_EXPR;
	  break;
	case CPP_OR_EQ:
	  opcode = BIT_IOR_EXPR;
	  break;
	case CPP_XOR_EQ:
	  opcode = BIT_XOR_EXPR;
	  break;
	case CPP_EQ:
	  c_parser_consume_token (parser);
	  eloc = c_parser_peek_token (parser)->location;
	  expr = c_parser_expr_no_commas (parser, NULL, unfolded_lhs);
	  rhs1 = expr.value;
	  switch (TREE_CODE (rhs1))
	    {
	    case MULT_EXPR:
	    case TRUNC_DIV_EXPR:
	    case PLUS_EXPR:
	    case MINUS_EXPR:
	    case LSHIFT_EXPR:
	    case RSHIFT_EXPR:
	    case BIT_AND_EXPR:
	    case BIT_IOR_EXPR:
	    case BIT_XOR_EXPR:
	      if (c_tree_equal (TREE_OPERAND (rhs1, 0), unfolded_lhs))
		{
		  opcode = TREE_CODE (rhs1);
		  rhs = c_fully_fold (TREE_OPERAND (rhs1, 1), false, NULL);
		  rhs1 = c_fully_fold (TREE_OPERAND (rhs1, 0), false, NULL);
		  goto stmt_done;
		}
	      if (c_tree_equal (TREE_OPERAND (rhs1, 1), unfolded_lhs))
		{
		  opcode = TREE_CODE (rhs1);
		  rhs = c_fully_fold (TREE_OPERAND (rhs1, 0), false, NULL);
		  rhs1 = c_fully_fold (TREE_OPERAND (rhs1, 1), false, NULL);
		  swapped = !commutative_tree_code (opcode);
		  goto stmt_done;
		}
	      break;
	    case ERROR_MARK:
	      goto saw_error;
	    default:
	      break;
	    }
	  if (c_parser_peek_token (parser)->type == CPP_SEMICOLON)
	    {
	      if (structured_block && code == OMP_ATOMIC_CAPTURE_NEW)
		{
		  code = OMP_ATOMIC_CAPTURE_OLD;
		  v = lhs;
		  lhs = NULL_TREE;
		  expr = default_function_array_read_conversion (eloc, expr);
		  unfolded_lhs1 = expr.value;
		  lhs1 = c_fully_fold (unfolded_lhs1, false, NULL);
		  rhs1 = NULL_TREE;
		  c_parser_consume_token (parser);
		  goto restart;
		}
	      if (structured_block)
		{
		  opcode = NOP_EXPR;
		  expr = default_function_array_read_conversion (eloc, expr);
		  rhs = c_fully_fold (expr.value, false, NULL);
		  rhs1 = NULL_TREE;
		  goto stmt_done;
		}
	    }
	  c_parser_error (parser, "invalid form of %<#pragma omp atomic%>");
	  goto saw_error;
	default:
	  c_parser_error (parser,
			  "invalid operator for %<#pragma omp atomic%>");
	  goto saw_error;
	}

      /* Arrange to pass the location of the assignment operator to
	 c_finish_omp_atomic.  */
      loc = c_parser_peek_token (parser)->location;
      c_parser_consume_token (parser);
      eloc = c_parser_peek_token (parser)->location;
      expr = c_parser_expression (parser);
      expr = default_function_array_read_conversion (eloc, expr);
      rhs = expr.value;
      rhs = c_fully_fold (rhs, false, NULL);
      break;
    }
stmt_done:
  if (structured_block && code == OMP_ATOMIC_CAPTURE_NEW)
    {
      if (!c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>"))
	goto saw_error;
      v = c_parser_unary_expression (parser).value;
      v = c_fully_fold (v, false, NULL);
      if (v == error_mark_node)
	goto saw_error;
      if (!c_parser_require (parser, CPP_EQ, "expected %<=%>"))
	goto saw_error;
      eloc = c_parser_peek_token (parser)->location;
      expr = c_parser_unary_expression (parser);
      lhs1 = expr.value;
      expr = default_function_array_read_conversion (eloc, expr);
      unfolded_lhs1 = expr.value;
      lhs1 = c_fully_fold (lhs1, false, NULL);
      if (lhs1 == error_mark_node)
	goto saw_error;
    }
  if (structured_block)
    {
      c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
      c_parser_require (parser, CPP_CLOSE_BRACE, "expected %<}%>");
    }
done:
  if (unfolded_lhs && unfolded_lhs1
      && !c_tree_equal (unfolded_lhs, unfolded_lhs1))
    {
      error ("%<#pragma omp atomic capture%> uses two different "
	     "expressions for memory");
      stmt = error_mark_node;
    }
  else
    stmt = c_finish_omp_atomic (loc, code, opcode, lhs, rhs, v, lhs1, rhs1,
				swapped, seq_cst);
  if (stmt != error_mark_node)
    add_stmt (stmt);

  if (!structured_block)
    c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
}


/* OpenMP 2.5:
   # pragma omp barrier new-line
*/

static void
c_parser_omp_barrier (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_consume_pragma (parser);
  c_parser_skip_to_pragma_eol (parser);

  c_finish_omp_barrier (loc);
}

/* OpenMP 2.5:
   # pragma omp critical [(name)] new-line
     structured-block

  LOC is the location of the #pragma itself.  */

static tree
c_parser_omp_critical (location_t loc, c_parser *parser)
{
  tree stmt, name = NULL;

  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  name = c_parser_peek_token (parser)->value;
	  c_parser_consume_token (parser);
	  c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>");
	}
      else
	c_parser_error (parser, "expected identifier");
    }
  else if (c_parser_next_token_is_not (parser, CPP_PRAGMA_EOL))
    c_parser_error (parser, "expected %<(%> or end of line");
  c_parser_skip_to_pragma_eol (parser);

  stmt = c_parser_omp_structured_block (parser);
  return c_finish_omp_critical (loc, stmt, name);
}

/* OpenMP 2.5:
   # pragma omp flush flush-vars[opt] new-line

   flush-vars:
     ( variable-list ) */

static void
c_parser_omp_flush (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_consume_pragma (parser);
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    c_parser_omp_var_list_parens (parser, OMP_CLAUSE_ERROR, NULL);
  else if (c_parser_next_token_is_not (parser, CPP_PRAGMA_EOL))
    c_parser_error (parser, "expected %<(%> or end of line");
  c_parser_skip_to_pragma_eol (parser);

  c_finish_omp_flush (loc);
}

/* Parse the restricted form of the for statement allowed by OpenMP.
   The real trick here is to determine the loop control variable early
   so that we can push a new decl if necessary to make it private.
   LOC is the location of the OMP in "#pragma omp".  */

static tree
c_parser_omp_for_loop (location_t loc, c_parser *parser, enum tree_code code,
		       tree clauses, tree *cclauses)
{
  tree decl, cond, incr, save_break, save_cont, body, init, stmt, cl;
  tree declv, condv, incrv, initv, ret = NULL;
  bool fail = false, open_brace_parsed = false;
  int i, collapse = 1, nbraces = 0;
  location_t for_loc;
  vec<tree, va_gc> *for_block = make_tree_vector ();

  for (cl = clauses; cl; cl = OMP_CLAUSE_CHAIN (cl))
    if (OMP_CLAUSE_CODE (cl) == OMP_CLAUSE_COLLAPSE)
      collapse = tree_to_shwi (OMP_CLAUSE_COLLAPSE_EXPR (cl));

  gcc_assert (collapse >= 1);

  declv = make_tree_vec (collapse);
  initv = make_tree_vec (collapse);
  condv = make_tree_vec (collapse);
  incrv = make_tree_vec (collapse);

  if (code != CILK_FOR
      && !c_parser_next_token_is_keyword (parser, RID_FOR))
    {
      c_parser_error (parser, "for statement expected");
      return NULL;
    }
  if (code == CILK_FOR
      && !c_parser_next_token_is_keyword (parser, RID_CILK_FOR))
    {
      c_parser_error (parser, "_Cilk_for statement expected");
      return NULL;
    }
  for_loc = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);

  for (i = 0; i < collapse; i++)
    {
      int bracecount = 0;

      if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	goto pop_scopes;

      /* Parse the initialization declaration or expression.  */
      if (c_parser_next_tokens_start_declaration (parser))
	{
	  if (i > 0)
	    vec_safe_push (for_block, c_begin_compound_stmt (true));
	  c_parser_declaration_or_fndef (parser, true, true, true, true, true,
					 NULL, vNULL);
	  decl = check_for_loop_decls (for_loc, flag_isoc99);
	  if (decl == NULL)
	    goto error_init;
	  if (DECL_INITIAL (decl) == error_mark_node)
	    decl = error_mark_node;
	  init = decl;
	}
      else if (c_parser_next_token_is (parser, CPP_NAME)
	       && c_parser_peek_2nd_token (parser)->type == CPP_EQ)
	{
	  struct c_expr decl_exp;
	  struct c_expr init_exp;
	  location_t init_loc;

	  decl_exp = c_parser_postfix_expression (parser);
	  decl = decl_exp.value;

	  c_parser_require (parser, CPP_EQ, "expected %<=%>");

	  init_loc = c_parser_peek_token (parser)->location;
	  init_exp = c_parser_expr_no_commas (parser, NULL);
	  init_exp = default_function_array_read_conversion (init_loc,
							     init_exp);
	  init = build_modify_expr (init_loc, decl, decl_exp.original_type,
				    NOP_EXPR, init_loc, init_exp.value,
				    init_exp.original_type);
	  init = c_process_expr_stmt (init_loc, init);

	  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
	}
      else
	{
	error_init:
	  c_parser_error (parser,
			  "expected iteration declaration or initialization");
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  fail = true;
	  goto parse_next;
	}

      /* Parse the loop condition.  */
      cond = NULL_TREE;
      if (c_parser_next_token_is_not (parser, CPP_SEMICOLON))
	{
	  location_t cond_loc = c_parser_peek_token (parser)->location;
	  struct c_expr cond_expr
	    = c_parser_binary_expression (parser, NULL, NULL_TREE);

	  cond = cond_expr.value;
	  cond = c_objc_common_truthvalue_conversion (cond_loc, cond);
	  cond = c_fully_fold (cond, false, NULL);
	  switch (cond_expr.original_code)
	    {
	    case GT_EXPR:
	    case GE_EXPR:
	    case LT_EXPR:
	    case LE_EXPR:
	      break;
	    case NE_EXPR:
	      if (code == CILK_SIMD || code == CILK_FOR)
		break;
	      /* FALLTHRU.  */
	    default:
	      /* Can't be cond = error_mark_node, because we want to preserve
		 the location until c_finish_omp_for.  */
	      cond = build1 (NOP_EXPR, boolean_type_node, error_mark_node);
	      break;
	    }
	  protected_set_expr_location (cond, cond_loc);
	}
      c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");

      /* Parse the increment expression.  */
      incr = NULL_TREE;
      if (c_parser_next_token_is_not (parser, CPP_CLOSE_PAREN))
	{
	  location_t incr_loc = c_parser_peek_token (parser)->location;

	  incr = c_process_expr_stmt (incr_loc,
				      c_parser_expression (parser).value);
	}
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");

      if (decl == NULL || decl == error_mark_node || init == error_mark_node)
	fail = true;
      else
	{
	  TREE_VEC_ELT (declv, i) = decl;
	  TREE_VEC_ELT (initv, i) = init;
	  TREE_VEC_ELT (condv, i) = cond;
	  TREE_VEC_ELT (incrv, i) = incr;
	}

    parse_next:
      if (i == collapse - 1)
	break;

      /* FIXME: OpenMP 3.0 draft isn't very clear on what exactly is allowed
	 in between the collapsed for loops to be still considered perfectly
	 nested.  Hopefully the final version clarifies this.
	 For now handle (multiple) {'s and empty statements.  */
      do
	{
	  if (c_parser_next_token_is_keyword (parser, RID_FOR))
	    {
	      c_parser_consume_token (parser);
	      break;
	    }
	  else if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	    {
	      c_parser_consume_token (parser);
	      bracecount++;
	    }
	  else if (bracecount
		   && c_parser_next_token_is (parser, CPP_SEMICOLON))
	    c_parser_consume_token (parser);
	  else
	    {
	      c_parser_error (parser, "not enough perfectly nested loops");
	      if (bracecount)
		{
		  open_brace_parsed = true;
		  bracecount--;
		}
	      fail = true;
	      collapse = 0;
	      break;
	    }
	}
      while (1);

      nbraces += bracecount;
    }

  save_break = c_break_label;
  if (code == CILK_SIMD)
    c_break_label = build_int_cst (size_type_node, 2);
  else
    c_break_label = size_one_node;
  save_cont = c_cont_label;
  c_cont_label = NULL_TREE;
  body = push_stmt_list ();

  if (open_brace_parsed)
    {
      location_t here = c_parser_peek_token (parser)->location;
      stmt = c_begin_compound_stmt (true);
      c_parser_compound_statement_nostart (parser);
      add_stmt (c_end_compound_stmt (here, stmt, true));
    }
  else
    add_stmt (c_parser_c99_block_statement (parser));
  if (c_cont_label)
    {
      tree t = build1 (LABEL_EXPR, void_type_node, c_cont_label);
      SET_EXPR_LOCATION (t, loc);
      add_stmt (t);
    }

  body = pop_stmt_list (body);
  c_break_label = save_break;
  c_cont_label = save_cont;

  while (nbraces)
    {
      if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	{
	  c_parser_consume_token (parser);
	  nbraces--;
	}
      else if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	c_parser_consume_token (parser);
      else
	{
	  c_parser_error (parser, "collapsed loops not perfectly nested");
	  while (nbraces)
	    {
	      location_t here = c_parser_peek_token (parser)->location;
	      stmt = c_begin_compound_stmt (true);
	      add_stmt (body);
	      c_parser_compound_statement_nostart (parser);
	      body = c_end_compound_stmt (here, stmt, true);
	      nbraces--;
	    }
	  goto pop_scopes;
	}
    }

  /* Only bother calling c_finish_omp_for if we haven't already generated
     an error from the initialization parsing.  */
  if (!fail)
    {
      stmt = c_finish_omp_for (loc, code, declv, initv, condv,
			       incrv, body, NULL);
      if (stmt)
	{
	  if (cclauses != NULL
	      && cclauses[C_OMP_CLAUSE_SPLIT_PARALLEL] != NULL)
	    {
	      tree *c;
	      for (c = &cclauses[C_OMP_CLAUSE_SPLIT_PARALLEL]; *c ; )
		if (OMP_CLAUSE_CODE (*c) != OMP_CLAUSE_FIRSTPRIVATE
		    && OMP_CLAUSE_CODE (*c) != OMP_CLAUSE_LASTPRIVATE)
		  c = &OMP_CLAUSE_CHAIN (*c);
		else
		  {
		    for (i = 0; i < collapse; i++)
		      if (TREE_VEC_ELT (declv, i) == OMP_CLAUSE_DECL (*c))
			break;
		    if (i == collapse)
		      c = &OMP_CLAUSE_CHAIN (*c);
		    else if (OMP_CLAUSE_CODE (*c) == OMP_CLAUSE_FIRSTPRIVATE)
		      {
			error_at (loc,
				  "iteration variable %qD should not be firstprivate",
				  OMP_CLAUSE_DECL (*c));
			*c = OMP_CLAUSE_CHAIN (*c);
		      }
		    else
		      {
			/* Copy lastprivate (decl) clause to OMP_FOR_CLAUSES,
			   change it to shared (decl) in
			   OMP_PARALLEL_CLAUSES.  */
			tree l = build_omp_clause (OMP_CLAUSE_LOCATION (*c),
						   OMP_CLAUSE_LASTPRIVATE);
			OMP_CLAUSE_DECL (l) = OMP_CLAUSE_DECL (*c);
			if (code == OMP_SIMD)
			  {
			    OMP_CLAUSE_CHAIN (l)
			      = cclauses[C_OMP_CLAUSE_SPLIT_FOR];
			    cclauses[C_OMP_CLAUSE_SPLIT_FOR] = l;
			  }
			else
			  {
			    OMP_CLAUSE_CHAIN (l) = clauses;
			    clauses = l;
			  }
			OMP_CLAUSE_SET_CODE (*c, OMP_CLAUSE_SHARED);
		      }
		  }
	    }
	  OMP_FOR_CLAUSES (stmt) = clauses;
	}
      ret = stmt;
    }
pop_scopes:
  while (!for_block->is_empty ())
    {
      /* FIXME diagnostics: LOC below should be the actual location of
	 this particular for block.  We need to build a list of
	 locations to go along with FOR_BLOCK.  */
      stmt = c_end_compound_stmt (loc, for_block->pop (), true);
      add_stmt (stmt);
    }
  release_tree_vector (for_block);
  return ret;
}

/* Helper function for OpenMP parsing, split clauses and call
   finish_omp_clauses on each of the set of clauses afterwards.  */

static void
omp_split_clauses (location_t loc, enum tree_code code,
		   omp_clause_mask mask, tree clauses, tree *cclauses)
{
  int i;
  c_omp_split_clauses (loc, code, mask, clauses, cclauses);
  for (i = 0; i < C_OMP_CLAUSE_SPLIT_COUNT; i++)
    if (cclauses[i])
      cclauses[i] = c_finish_omp_clauses (cclauses[i]);
}

/* OpenMP 4.0:
   #pragma omp simd simd-clause[optseq] new-line
     for-loop

   LOC is the location of the #pragma token.
*/

#define OMP_SIMD_CLAUSE_MASK					\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SAFELEN)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_LINEAR)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_ALIGNED)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_LASTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_REDUCTION)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_COLLAPSE))

static tree
c_parser_omp_simd (location_t loc, c_parser *parser,
		   char *p_name, omp_clause_mask mask, tree *cclauses)
{
  tree block, clauses, ret;

  strcat (p_name, " simd");
  mask |= OMP_SIMD_CLAUSE_MASK;
  mask &= ~(OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_ORDERED);

  clauses = c_parser_omp_all_clauses (parser, mask, p_name, cclauses == NULL);
  if (cclauses)
    {
      omp_split_clauses (loc, OMP_SIMD, mask, clauses, cclauses);
      clauses = cclauses[C_OMP_CLAUSE_SPLIT_SIMD];
    }

  block = c_begin_compound_stmt (true);
  ret = c_parser_omp_for_loop (loc, parser, OMP_SIMD, clauses, cclauses);
  block = c_end_compound_stmt (loc, block, true);
  add_stmt (block);

  return ret;
}

/* OpenMP 2.5:
   #pragma omp for for-clause[optseq] new-line
     for-loop

   OpenMP 4.0:
   #pragma omp for simd for-simd-clause[optseq] new-line
     for-loop

   LOC is the location of the #pragma token.
*/

#define OMP_FOR_CLAUSE_MASK					\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FIRSTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_LASTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_REDUCTION)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_ORDERED)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SCHEDULE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_COLLAPSE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NOWAIT))

static tree
c_parser_omp_for (location_t loc, c_parser *parser,
		  char *p_name, omp_clause_mask mask, tree *cclauses)
{
  tree block, clauses, ret;

  strcat (p_name, " for");
  mask |= OMP_FOR_CLAUSE_MASK;
  if (cclauses)
    mask &= ~(OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NOWAIT);

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);

      if (strcmp (p, "simd") == 0)
	{
	  tree cclauses_buf[C_OMP_CLAUSE_SPLIT_COUNT];
	  if (cclauses == NULL)
	    cclauses = cclauses_buf;

	  c_parser_consume_token (parser);
	  if (!flag_openmp)  /* flag_openmp_simd  */
	    return c_parser_omp_simd (loc, parser, p_name, mask, cclauses);
	  block = c_begin_compound_stmt (true);
	  ret = c_parser_omp_simd (loc, parser, p_name, mask, cclauses);
	  block = c_end_compound_stmt (loc, block, true);
	  if (ret == NULL_TREE)
	    return ret;
	  ret = make_node (OMP_FOR);
	  TREE_TYPE (ret) = void_type_node;
	  OMP_FOR_BODY (ret) = block;
	  OMP_FOR_CLAUSES (ret) = cclauses[C_OMP_CLAUSE_SPLIT_FOR];
	  SET_EXPR_LOCATION (ret, loc);
	  add_stmt (ret);
	  return ret;
	}
    }
  if (!flag_openmp)  /* flag_openmp_simd  */
    {
      c_parser_skip_to_pragma_eol (parser);
      return NULL_TREE;
    }

  clauses = c_parser_omp_all_clauses (parser, mask, p_name, cclauses == NULL);
  if (cclauses)
    {
      omp_split_clauses (loc, OMP_FOR, mask, clauses, cclauses);
      clauses = cclauses[C_OMP_CLAUSE_SPLIT_FOR];
    }

  block = c_begin_compound_stmt (true);
  ret = c_parser_omp_for_loop (loc, parser, OMP_FOR, clauses, cclauses);
  block = c_end_compound_stmt (loc, block, true);
  add_stmt (block);

  return ret;
}

/* OpenMP 2.5:
   # pragma omp master new-line
     structured-block

   LOC is the location of the #pragma token.
*/

static tree
c_parser_omp_master (location_t loc, c_parser *parser)
{
  c_parser_skip_to_pragma_eol (parser);
  return c_finish_omp_master (loc, c_parser_omp_structured_block (parser));
}

/* OpenMP 2.5:
   # pragma omp ordered new-line
     structured-block

   LOC is the location of the #pragma itself.
*/

static tree
c_parser_omp_ordered (location_t loc, c_parser *parser)
{
  c_parser_skip_to_pragma_eol (parser);
  return c_finish_omp_ordered (loc, c_parser_omp_structured_block (parser));
}

/* OpenMP 2.5:

   section-scope:
     { section-sequence }

   section-sequence:
     section-directive[opt] structured-block
     section-sequence section-directive structured-block

    SECTIONS_LOC is the location of the #pragma omp sections.  */

static tree
c_parser_omp_sections_scope (location_t sections_loc, c_parser *parser)
{
  tree stmt, substmt;
  bool error_suppress = false;
  location_t loc;

  loc = c_parser_peek_token (parser)->location;
  if (!c_parser_require (parser, CPP_OPEN_BRACE, "expected %<{%>"))
    {
      /* Avoid skipping until the end of the block.  */
      parser->error = false;
      return NULL_TREE;
    }

  stmt = push_stmt_list ();

  if (c_parser_peek_token (parser)->pragma_kind != PRAGMA_OMP_SECTION)
    {
      substmt = c_parser_omp_structured_block (parser);
      substmt = build1 (OMP_SECTION, void_type_node, substmt);
      SET_EXPR_LOCATION (substmt, loc);
      add_stmt (substmt);
    }

  while (1)
    {
      if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	break;
      if (c_parser_next_token_is (parser, CPP_EOF))
	break;

      loc = c_parser_peek_token (parser)->location;
      if (c_parser_peek_token (parser)->pragma_kind == PRAGMA_OMP_SECTION)
	{
	  c_parser_consume_pragma (parser);
	  c_parser_skip_to_pragma_eol (parser);
	  error_suppress = false;
	}
      else if (!error_suppress)
	{
	  error_at (loc, "expected %<#pragma omp section%> or %<}%>");
	  error_suppress = true;
	}

      substmt = c_parser_omp_structured_block (parser);
      substmt = build1 (OMP_SECTION, void_type_node, substmt);
      SET_EXPR_LOCATION (substmt, loc);
      add_stmt (substmt);
    }
  c_parser_skip_until_found (parser, CPP_CLOSE_BRACE,
			     "expected %<#pragma omp section%> or %<}%>");

  substmt = pop_stmt_list (stmt);

  stmt = make_node (OMP_SECTIONS);
  SET_EXPR_LOCATION (stmt, sections_loc);
  TREE_TYPE (stmt) = void_type_node;
  OMP_SECTIONS_BODY (stmt) = substmt;

  return add_stmt (stmt);
}

/* OpenMP 2.5:
   # pragma omp sections sections-clause[optseq] newline
     sections-scope

   LOC is the location of the #pragma token.
*/

#define OMP_SECTIONS_CLAUSE_MASK				\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FIRSTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_LASTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_REDUCTION)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NOWAIT))

static tree
c_parser_omp_sections (location_t loc, c_parser *parser,
		       char *p_name, omp_clause_mask mask, tree *cclauses)
{
  tree block, clauses, ret;

  strcat (p_name, " sections");
  mask |= OMP_SECTIONS_CLAUSE_MASK;
  if (cclauses)
    mask &= ~(OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NOWAIT);

  clauses = c_parser_omp_all_clauses (parser, mask, p_name, cclauses == NULL);
  if (cclauses)
    {
      omp_split_clauses (loc, OMP_SECTIONS, mask, clauses, cclauses);
      clauses = cclauses[C_OMP_CLAUSE_SPLIT_SECTIONS];
    }

  block = c_begin_compound_stmt (true);
  ret = c_parser_omp_sections_scope (loc, parser);
  if (ret)
    OMP_SECTIONS_CLAUSES (ret) = clauses;
  block = c_end_compound_stmt (loc, block, true);
  add_stmt (block);

  return ret;
}

/* OpenMP 2.5:
   # pragma omp parallel parallel-clause[optseq] new-line
     structured-block
   # pragma omp parallel for parallel-for-clause[optseq] new-line
     structured-block
   # pragma omp parallel sections parallel-sections-clause[optseq] new-line
     structured-block

   OpenMP 4.0:
   # pragma omp parallel for simd parallel-for-simd-clause[optseq] new-line
     structured-block

   LOC is the location of the #pragma token.
*/

#define OMP_PARALLEL_CLAUSE_MASK				\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_IF)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FIRSTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DEFAULT)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SHARED)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_COPYIN)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_REDUCTION)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NUM_THREADS)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PROC_BIND))

static tree
c_parser_omp_parallel (location_t loc, c_parser *parser,
		       char *p_name, omp_clause_mask mask, tree *cclauses)
{
  tree stmt, clauses, block;

  strcat (p_name, " parallel");
  mask |= OMP_PARALLEL_CLAUSE_MASK;

  if (c_parser_next_token_is_keyword (parser, RID_FOR))
    {
      tree cclauses_buf[C_OMP_CLAUSE_SPLIT_COUNT];
      if (cclauses == NULL)
	cclauses = cclauses_buf;

      c_parser_consume_token (parser);
      if (!flag_openmp)  /* flag_openmp_simd  */
	return c_parser_omp_for (loc, parser, p_name, mask, cclauses);
      block = c_begin_omp_parallel ();
      tree ret = c_parser_omp_for (loc, parser, p_name, mask, cclauses);
      stmt
	= c_finish_omp_parallel (loc, cclauses[C_OMP_CLAUSE_SPLIT_PARALLEL],
				 block);
      if (ret == NULL_TREE)
	return ret;
      OMP_PARALLEL_COMBINED (stmt) = 1;
      return stmt;
    }
  else if (cclauses)
    {
      error_at (loc, "expected %<for%> after %qs", p_name);
      c_parser_skip_to_pragma_eol (parser);
      return NULL_TREE;
    }
  else if (!flag_openmp)  /* flag_openmp_simd  */
    {
      c_parser_skip_to_pragma_eol (parser);
      return NULL_TREE;
    }
  else if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp (p, "sections") == 0)
	{
	  tree cclauses_buf[C_OMP_CLAUSE_SPLIT_COUNT];
	  if (cclauses == NULL)
	    cclauses = cclauses_buf;

	  c_parser_consume_token (parser);
	  block = c_begin_omp_parallel ();
	  c_parser_omp_sections (loc, parser, p_name, mask, cclauses);
	  stmt = c_finish_omp_parallel (loc,
					cclauses[C_OMP_CLAUSE_SPLIT_PARALLEL],
					block);
	  OMP_PARALLEL_COMBINED (stmt) = 1;
	  return stmt;
	}
    }

  clauses = c_parser_omp_all_clauses (parser, mask, p_name, cclauses == NULL);

  block = c_begin_omp_parallel ();
  c_parser_statement (parser);
  stmt = c_finish_omp_parallel (loc, clauses, block);

  return stmt;
}

/* OpenMP 2.5:
   # pragma omp single single-clause[optseq] new-line
     structured-block

   LOC is the location of the #pragma.
*/

#define OMP_SINGLE_CLAUSE_MASK					\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FIRSTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_COPYPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NOWAIT))

static tree
c_parser_omp_single (location_t loc, c_parser *parser)
{
  tree stmt = make_node (OMP_SINGLE);
  SET_EXPR_LOCATION (stmt, loc);
  TREE_TYPE (stmt) = void_type_node;

  OMP_SINGLE_CLAUSES (stmt)
    = c_parser_omp_all_clauses (parser, OMP_SINGLE_CLAUSE_MASK,
				"#pragma omp single");
  OMP_SINGLE_BODY (stmt) = c_parser_omp_structured_block (parser);

  return add_stmt (stmt);
}

/* OpenMP 3.0:
   # pragma omp task task-clause[optseq] new-line

   LOC is the location of the #pragma.
*/

#define OMP_TASK_CLAUSE_MASK					\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_IF)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_UNTIED)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DEFAULT)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FIRSTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SHARED)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FINAL)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_MERGEABLE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DEPEND))

static tree
c_parser_omp_task (location_t loc, c_parser *parser)
{
  tree clauses, block;

  clauses = c_parser_omp_all_clauses (parser, OMP_TASK_CLAUSE_MASK,
				      "#pragma omp task");

  block = c_begin_omp_task ();
  c_parser_statement (parser);
  return c_finish_omp_task (loc, clauses, block);
}

/* OpenMP 3.0:
   # pragma omp taskwait new-line
*/

static void
c_parser_omp_taskwait (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_consume_pragma (parser);
  c_parser_skip_to_pragma_eol (parser);

  c_finish_omp_taskwait (loc);
}

/* OpenMP 3.1:
   # pragma omp taskyield new-line
*/

static void
c_parser_omp_taskyield (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_consume_pragma (parser);
  c_parser_skip_to_pragma_eol (parser);

  c_finish_omp_taskyield (loc);
}

/* OpenMP 4.0:
   # pragma omp taskgroup new-line
*/

static tree
c_parser_omp_taskgroup (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_skip_to_pragma_eol (parser);
  return c_finish_omp_taskgroup (loc, c_parser_omp_structured_block (parser));
}

/* OpenMP 4.0:
   # pragma omp cancel cancel-clause[optseq] new-line

   LOC is the location of the #pragma.
*/

#define OMP_CANCEL_CLAUSE_MASK					\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PARALLEL)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FOR)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SECTIONS)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_TASKGROUP)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_IF))

static void
c_parser_omp_cancel (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;

  c_parser_consume_pragma (parser);
  tree clauses = c_parser_omp_all_clauses (parser, OMP_CANCEL_CLAUSE_MASK,
					   "#pragma omp cancel");

  c_finish_omp_cancel (loc, clauses);
}

/* OpenMP 4.0:
   # pragma omp cancellation point cancelpt-clause[optseq] new-line

   LOC is the location of the #pragma.
*/

#define OMP_CANCELLATION_POINT_CLAUSE_MASK			\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PARALLEL)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FOR)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SECTIONS)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_TASKGROUP))

static void
c_parser_omp_cancellation_point (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  tree clauses;
  bool point_seen = false;

  c_parser_consume_pragma (parser);
  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp (p, "point") == 0)
	{
	  c_parser_consume_token (parser);
	  point_seen = true;
	}
    }
  if (!point_seen)
    {
      c_parser_error (parser, "expected %<point%>");
      c_parser_skip_to_pragma_eol (parser);
      return;
    }

  clauses
    = c_parser_omp_all_clauses (parser, OMP_CANCELLATION_POINT_CLAUSE_MASK,
				"#pragma omp cancellation point");

  c_finish_omp_cancellation_point (loc, clauses);
}

/* OpenMP 4.0:
   #pragma omp distribute distribute-clause[optseq] new-line
     for-loop  */

#define OMP_DISTRIBUTE_CLAUSE_MASK				\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FIRSTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DIST_SCHEDULE)\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_COLLAPSE))

static tree
c_parser_omp_distribute (location_t loc, c_parser *parser,
			 char *p_name, omp_clause_mask mask, tree *cclauses)
{
  tree clauses, block, ret;

  strcat (p_name, " distribute");
  mask |= OMP_DISTRIBUTE_CLAUSE_MASK;

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      bool simd = false;
      bool parallel = false;

      if (strcmp (p, "simd") == 0)
	simd = true;
      else
	parallel = strcmp (p, "parallel") == 0;
      if (parallel || simd)
	{
	  tree cclauses_buf[C_OMP_CLAUSE_SPLIT_COUNT];
	  if (cclauses == NULL)
	    cclauses = cclauses_buf;
	  c_parser_consume_token (parser);
	  if (!flag_openmp)  /* flag_openmp_simd  */
	    {
	      if (simd)
		return c_parser_omp_simd (loc, parser, p_name, mask, cclauses);
	      else
		return c_parser_omp_parallel (loc, parser, p_name, mask,
					      cclauses);
	    }
	  block = c_begin_compound_stmt (true);
	  if (simd)
	    ret = c_parser_omp_simd (loc, parser, p_name, mask, cclauses);
	  else
	    ret = c_parser_omp_parallel (loc, parser, p_name, mask, cclauses);
	  block = c_end_compound_stmt (loc, block, true);
	  if (ret == NULL)
	    return ret;
	  ret = make_node (OMP_DISTRIBUTE);
	  TREE_TYPE (ret) = void_type_node;
	  OMP_FOR_BODY (ret) = block;
	  OMP_FOR_CLAUSES (ret) = cclauses[C_OMP_CLAUSE_SPLIT_DISTRIBUTE];
	  SET_EXPR_LOCATION (ret, loc);
	  add_stmt (ret);
	  return ret;
	}
    }
  if (!flag_openmp)  /* flag_openmp_simd  */
    {
      c_parser_skip_to_pragma_eol (parser);
      return NULL_TREE;
    }

  clauses = c_parser_omp_all_clauses (parser, mask, p_name, cclauses == NULL);
  if (cclauses)
    {
      omp_split_clauses (loc, OMP_DISTRIBUTE, mask, clauses, cclauses);
      clauses = cclauses[C_OMP_CLAUSE_SPLIT_DISTRIBUTE];
    }

  block = c_begin_compound_stmt (true);
  ret = c_parser_omp_for_loop (loc, parser, OMP_DISTRIBUTE, clauses, NULL);
  block = c_end_compound_stmt (loc, block, true);
  add_stmt (block);

  return ret;
}

/* OpenMP 4.0:
   # pragma omp teams teams-clause[optseq] new-line
     structured-block  */

#define OMP_TEAMS_CLAUSE_MASK					\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_PRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FIRSTPRIVATE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SHARED)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_REDUCTION)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NUM_TEAMS)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_THREAD_LIMIT)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DEFAULT))

static tree
c_parser_omp_teams (location_t loc, c_parser *parser,
		    char *p_name, omp_clause_mask mask, tree *cclauses)
{
  tree clauses, block, ret;

  strcat (p_name, " teams");
  mask |= OMP_TEAMS_CLAUSE_MASK;

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp (p, "distribute") == 0)
	{
	  tree cclauses_buf[C_OMP_CLAUSE_SPLIT_COUNT];
	  if (cclauses == NULL)
	    cclauses = cclauses_buf;

	  c_parser_consume_token (parser);
	  if (!flag_openmp)  /* flag_openmp_simd  */
	    return c_parser_omp_distribute (loc, parser, p_name, mask, cclauses);
	  block = c_begin_compound_stmt (true);
	  ret = c_parser_omp_distribute (loc, parser, p_name, mask, cclauses);
	  block = c_end_compound_stmt (loc, block, true);
	  if (ret == NULL)
	    return ret;
	  clauses = cclauses[C_OMP_CLAUSE_SPLIT_TEAMS];
	  ret = make_node (OMP_TEAMS);
	  TREE_TYPE (ret) = void_type_node;
	  OMP_TEAMS_CLAUSES (ret) = clauses;
	  OMP_TEAMS_BODY (ret) = block;
	  return add_stmt (ret);
	}
    }
  if (!flag_openmp)  /* flag_openmp_simd  */
    {
      c_parser_skip_to_pragma_eol (parser);
      return NULL_TREE;
    }

  clauses = c_parser_omp_all_clauses (parser, mask, p_name, cclauses == NULL);
  if (cclauses)
    {
      omp_split_clauses (loc, OMP_TEAMS, mask, clauses, cclauses);
      clauses = cclauses[C_OMP_CLAUSE_SPLIT_TEAMS];
    }

  tree stmt = make_node (OMP_TEAMS);
  TREE_TYPE (stmt) = void_type_node;
  OMP_TEAMS_CLAUSES (stmt) = clauses;
  OMP_TEAMS_BODY (stmt) = c_parser_omp_structured_block (parser);

  return add_stmt (stmt);
}

/* OpenMP 4.0:
   # pragma omp target data target-data-clause[optseq] new-line
     structured-block  */

#define OMP_TARGET_DATA_CLAUSE_MASK				\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DEVICE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_MAP)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_IF))

static tree
c_parser_omp_target_data (location_t loc, c_parser *parser)
{
  tree stmt = make_node (OMP_TARGET_DATA);
  TREE_TYPE (stmt) = void_type_node;

  OMP_TARGET_DATA_CLAUSES (stmt)
    = c_parser_omp_all_clauses (parser, OMP_TARGET_DATA_CLAUSE_MASK,
				"#pragma omp target data");
  keep_next_level ();
  tree block = c_begin_compound_stmt (true);
  add_stmt (c_parser_omp_structured_block (parser));
  OMP_TARGET_DATA_BODY (stmt) = c_end_compound_stmt (loc, block, true);

  SET_EXPR_LOCATION (stmt, loc);
  return add_stmt (stmt);
}

/* OpenMP 4.0:
   # pragma omp target update target-update-clause[optseq] new-line */

#define OMP_TARGET_UPDATE_CLAUSE_MASK				\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_FROM)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_TO)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DEVICE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_IF))

static bool
c_parser_omp_target_update (location_t loc, c_parser *parser,
			    enum pragma_context context)
{
  if (context == pragma_stmt)
    {
      error_at (loc,
		"%<#pragma omp target update%> may only be "
		"used in compound statements");
      c_parser_skip_to_pragma_eol (parser);
      return false;
    }

  tree clauses
    = c_parser_omp_all_clauses (parser, OMP_TARGET_UPDATE_CLAUSE_MASK,
				"#pragma omp target update");
  if (find_omp_clause (clauses, OMP_CLAUSE_TO) == NULL_TREE
      && find_omp_clause (clauses, OMP_CLAUSE_FROM) == NULL_TREE)
    {
      error_at (loc,
		"%<#pragma omp target update must contain at least one "
		"%<from%> or %<to%> clauses");
      return false;
    }

  tree stmt = make_node (OMP_TARGET_UPDATE);
  TREE_TYPE (stmt) = void_type_node;
  OMP_TARGET_UPDATE_CLAUSES (stmt) = clauses;
  SET_EXPR_LOCATION (stmt, loc);
  add_stmt (stmt);
  return false;
}

/* OpenMP 4.0:
   # pragma omp target target-clause[optseq] new-line
     structured-block  */

#define OMP_TARGET_CLAUSE_MASK					\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_DEVICE)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_MAP)		\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_IF))

static bool
c_parser_omp_target (c_parser *parser, enum pragma_context context)
{
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_consume_pragma (parser);

  if (context != pragma_stmt && context != pragma_compound)
    {
      c_parser_error (parser, "expected declaration specifiers");
      c_parser_skip_to_pragma_eol (parser);
      return false;
    }

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);

      if (strcmp (p, "teams") == 0)
	{
	  tree cclauses[C_OMP_CLAUSE_SPLIT_COUNT];
	  char p_name[sizeof ("#pragma omp target teams distribute "
			      "parallel for simd")];

	  c_parser_consume_token (parser);
	  strcpy (p_name, "#pragma omp target");
	  if (!flag_openmp)  /* flag_openmp_simd  */
	    {
	      tree stmt = c_parser_omp_teams (loc, parser, p_name,
					      OMP_TARGET_CLAUSE_MASK,
					      cclauses);
	      return stmt != NULL_TREE;
	    }
	  keep_next_level ();
	  tree block = c_begin_compound_stmt (true);
	  tree ret = c_parser_omp_teams (loc, parser, p_name,
					 OMP_TARGET_CLAUSE_MASK, cclauses);
	  block = c_end_compound_stmt (loc, block, true);
	  if (ret == NULL_TREE)
	    return false;
	  tree stmt = make_node (OMP_TARGET);
	  TREE_TYPE (stmt) = void_type_node;
	  OMP_TARGET_CLAUSES (stmt) = cclauses[C_OMP_CLAUSE_SPLIT_TARGET];
	  OMP_TARGET_BODY (stmt) = block;
	  add_stmt (stmt);
	  return true;
	}
      else if (!flag_openmp)  /* flag_openmp_simd  */
	{
	  c_parser_skip_to_pragma_eol (parser);
	  return false;
	}
      else if (strcmp (p, "data") == 0)
	{
	  c_parser_consume_token (parser);
	  c_parser_omp_target_data (loc, parser);
	  return true;
	}
      else if (strcmp (p, "update") == 0)
	{
	  c_parser_consume_token (parser);
	  return c_parser_omp_target_update (loc, parser, context);
	}
    }

  tree stmt = make_node (OMP_TARGET);
  TREE_TYPE (stmt) = void_type_node;

  OMP_TARGET_CLAUSES (stmt)
    = c_parser_omp_all_clauses (parser, OMP_TARGET_CLAUSE_MASK,
				"#pragma omp target");
  keep_next_level ();
  tree block = c_begin_compound_stmt (true);
  add_stmt (c_parser_omp_structured_block (parser));
  OMP_TARGET_BODY (stmt) = c_end_compound_stmt (loc, block, true);

  SET_EXPR_LOCATION (stmt, loc);
  add_stmt (stmt);
  return true;
}

/* OpenMP 4.0:
   # pragma omp declare simd declare-simd-clauses[optseq] new-line  */

#define OMP_DECLARE_SIMD_CLAUSE_MASK				\
	( (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_SIMDLEN)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_LINEAR)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_ALIGNED)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_UNIFORM)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_INBRANCH)	\
	| (OMP_CLAUSE_MASK_1 << PRAGMA_OMP_CLAUSE_NOTINBRANCH))

static void
c_parser_omp_declare_simd (c_parser *parser, enum pragma_context context)
{
  vec<c_token> clauses = vNULL;
  while (c_parser_next_token_is_not (parser, CPP_PRAGMA_EOL))
    {
      c_token *token = c_parser_peek_token (parser);
      if (token->type == CPP_EOF)
	{
	  c_parser_skip_to_pragma_eol (parser);
	  clauses.release ();
	  return;
	}
      clauses.safe_push (*token);
      c_parser_consume_token (parser);
    }
  clauses.safe_push (*c_parser_peek_token (parser));
  c_parser_skip_to_pragma_eol (parser);

  while (c_parser_next_token_is (parser, CPP_PRAGMA))
    {
      if (c_parser_peek_token (parser)->pragma_kind
	  != PRAGMA_OMP_DECLARE_REDUCTION
	  || c_parser_peek_2nd_token (parser)->type != CPP_NAME
	  || strcmp (IDENTIFIER_POINTER
				(c_parser_peek_2nd_token (parser)->value),
		     "simd") != 0)
	{
	  c_parser_error (parser,
			  "%<#pragma omp declare simd%> must be followed by "
			  "function declaration or definition or another "
			  "%<#pragma omp declare simd%>");
	  clauses.release ();
	  return;
	}
      c_parser_consume_pragma (parser);
      while (c_parser_next_token_is_not (parser, CPP_PRAGMA_EOL))
	{
	  c_token *token = c_parser_peek_token (parser);
	  if (token->type == CPP_EOF)
	    {
	      c_parser_skip_to_pragma_eol (parser);
	      clauses.release ();
	      return;
	    }
	  clauses.safe_push (*token);
	  c_parser_consume_token (parser);
	}
      clauses.safe_push (*c_parser_peek_token (parser));
      c_parser_skip_to_pragma_eol (parser);
    }

  /* Make sure nothing tries to read past the end of the tokens.  */
  c_token eof_token;
  memset (&eof_token, 0, sizeof (eof_token));
  eof_token.type = CPP_EOF;
  clauses.safe_push (eof_token);
  clauses.safe_push (eof_token);

  switch (context)
    {
    case pragma_external:
      if (c_parser_next_token_is (parser, CPP_KEYWORD)
	  && c_parser_peek_token (parser)->keyword == RID_EXTENSION)
	{
	  int ext = disable_extension_diagnostics ();
	  do
	    c_parser_consume_token (parser);
	  while (c_parser_next_token_is (parser, CPP_KEYWORD)
		 && c_parser_peek_token (parser)->keyword == RID_EXTENSION);
	  c_parser_declaration_or_fndef (parser, true, true, true, false, true,
					 NULL, clauses);
	  restore_extension_diagnostics (ext);
	}
      else
	c_parser_declaration_or_fndef (parser, true, true, true, false, true,
				       NULL, clauses);
      break;
    case pragma_struct:
    case pragma_param:
      c_parser_error (parser, "%<#pragma omp declare simd%> must be followed by "
			      "function declaration or definition");
      break;
    case pragma_compound:
    case pragma_stmt:
      if (c_parser_next_token_is (parser, CPP_KEYWORD)
	  && c_parser_peek_token (parser)->keyword == RID_EXTENSION)
	{
	  int ext = disable_extension_diagnostics ();
	  do
	    c_parser_consume_token (parser);
	  while (c_parser_next_token_is (parser, CPP_KEYWORD)
		 && c_parser_peek_token (parser)->keyword == RID_EXTENSION);
	  if (c_parser_next_tokens_start_declaration (parser))
	    {
	      c_parser_declaration_or_fndef (parser, true, true, true, true,
					     true, NULL, clauses);
	      restore_extension_diagnostics (ext);
	      break;
	    }
	  restore_extension_diagnostics (ext);
	}
      else if (c_parser_next_tokens_start_declaration (parser))
	{
	  c_parser_declaration_or_fndef (parser, true, true, true, true, true,
					 NULL, clauses);
	  break;
	}
      c_parser_error (parser, "%<#pragma omp declare simd%> must be followed by "
			      "function declaration or definition");
      break;
    default:
      gcc_unreachable ();
    }
  clauses.release ();
}

/* Finalize #pragma omp declare simd clauses after FNDECL has been parsed,
   and put that into "omp declare simd" attribute.  */

static void
c_finish_omp_declare_simd (c_parser *parser, tree fndecl, tree parms,
			   vec<c_token> clauses)
{
  if (flag_cilkplus
      && clauses.exists () && !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
    {
      error ("%<#pragma omp declare simd%> cannot be used in the same "
	     "function marked as a Cilk Plus SIMD-enabled function");
      vec_free (parser->cilk_simd_fn_tokens);
      return;
    }
  
  /* Normally first token is CPP_NAME "simd".  CPP_EOF there indicates
     error has been reported and CPP_PRAGMA that c_finish_omp_declare_simd
     has already processed the tokens.  */
  if (clauses.exists () && clauses[0].type == CPP_EOF)
    return;
  if (fndecl == NULL_TREE || TREE_CODE (fndecl) != FUNCTION_DECL)
    {
      error ("%<#pragma omp declare simd%> not immediately followed by "
	     "a function declaration or definition");
      clauses[0].type = CPP_EOF;
      return;
    }
  if (clauses.exists () && clauses[0].type != CPP_NAME)
    {
      error_at (DECL_SOURCE_LOCATION (fndecl),
		"%<#pragma omp declare simd%> not immediately followed by "
		"a single function declaration or definition");
      clauses[0].type = CPP_EOF;
      return;
    }

  if (parms == NULL_TREE)
    parms = DECL_ARGUMENTS (fndecl);

  unsigned int tokens_avail = parser->tokens_avail;
  gcc_assert (parser->tokens == &parser->tokens_buf[0]);
  bool is_cilkplus_cilk_simd_fn = false;
  
  if (flag_cilkplus && !vec_safe_is_empty (parser->cilk_simd_fn_tokens))
    {
      parser->tokens = parser->cilk_simd_fn_tokens->address ();
      parser->tokens_avail = vec_safe_length (parser->cilk_simd_fn_tokens);
      is_cilkplus_cilk_simd_fn = true;
    }
  else
    {
      parser->tokens = clauses.address ();
      parser->tokens_avail = clauses.length ();
    }
  
  /* c_parser_omp_declare_simd pushed 2 extra CPP_EOF tokens at the end.  */
  while (parser->tokens_avail > 3)
    {
      c_token *token = c_parser_peek_token (parser);
      if (!is_cilkplus_cilk_simd_fn) 
	gcc_assert (token->type == CPP_NAME 
		    && strcmp (IDENTIFIER_POINTER (token->value), "simd") == 0);
      else
	gcc_assert (token->type == CPP_NAME
		    && is_cilkplus_vector_p (token->value));
      c_parser_consume_token (parser);
      parser->in_pragma = true;

      tree c = NULL_TREE;
      if (is_cilkplus_cilk_simd_fn) 
	c = c_parser_omp_all_clauses (parser, CILK_SIMD_FN_CLAUSE_MASK,
				      "SIMD-enabled functions attribute");
      else 
	c = c_parser_omp_all_clauses (parser, OMP_DECLARE_SIMD_CLAUSE_MASK,
				      "#pragma omp declare simd");
      c = c_omp_declare_simd_clauses_to_numbers (parms, c);
      if (c != NULL_TREE)
	c = tree_cons (NULL_TREE, c, NULL_TREE);
      if (is_cilkplus_cilk_simd_fn) 
	{ 
	  tree k = build_tree_list (get_identifier ("cilk simd function"), 
				    NULL_TREE);
	  TREE_CHAIN (k) = DECL_ATTRIBUTES (fndecl);
	  DECL_ATTRIBUTES (fndecl) = k;
	} 
      c = build_tree_list (get_identifier ("omp declare simd"), c);
      TREE_CHAIN (c) = DECL_ATTRIBUTES (fndecl);
      DECL_ATTRIBUTES (fndecl) = c;
    }

  parser->tokens = &parser->tokens_buf[0];
  parser->tokens_avail = tokens_avail;
  if (clauses.exists ())
    clauses[0].type = CPP_PRAGMA;

  if (!vec_safe_is_empty (parser->cilk_simd_fn_tokens))
    vec_free (parser->cilk_simd_fn_tokens);
}


/* OpenMP 4.0:
   # pragma omp declare target new-line
   declarations and definitions
   # pragma omp end declare target new-line  */

static void
c_parser_omp_declare_target (c_parser *parser)
{
  c_parser_skip_to_pragma_eol (parser);
  current_omp_declare_target_attribute++;
}

static void
c_parser_omp_end_declare_target (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_consume_pragma (parser);
  if (c_parser_next_token_is (parser, CPP_NAME)
      && strcmp (IDENTIFIER_POINTER (c_parser_peek_token (parser)->value),
		 "declare") == 0)
    {
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_NAME)
	  && strcmp (IDENTIFIER_POINTER (c_parser_peek_token (parser)->value),
		     "target") == 0)
	c_parser_consume_token (parser);
      else
	{
	  c_parser_error (parser, "expected %<target%>");
	  c_parser_skip_to_pragma_eol (parser);
	  return;
	}
    }
  else
    {
      c_parser_error (parser, "expected %<declare%>");
      c_parser_skip_to_pragma_eol (parser);
      return;
    }
  c_parser_skip_to_pragma_eol (parser);
  if (!current_omp_declare_target_attribute)
    error_at (loc, "%<#pragma omp end declare target%> without corresponding "
		   "%<#pragma omp declare target%>");
  else
    current_omp_declare_target_attribute--;
}


/* OpenMP 4.0
   #pragma omp declare reduction (reduction-id : typename-list : expression) \
      initializer-clause[opt] new-line

   initializer-clause:
      initializer (omp_priv = initializer)
      initializer (function-name (argument-list))  */

static void
c_parser_omp_declare_reduction (c_parser *parser, enum pragma_context context)
{
  unsigned int tokens_avail = 0, i;
  vec<tree> types = vNULL;
  vec<c_token> clauses = vNULL;
  enum tree_code reduc_code = ERROR_MARK;
  tree reduc_id = NULL_TREE;
  tree type;
  location_t rloc = c_parser_peek_token (parser)->location;

  if (context == pragma_struct || context == pragma_param)
    {
      error ("%<#pragma omp declare reduction%> not at file or block scope");
      goto fail;
    }

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    goto fail;

  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_PLUS:
      reduc_code = PLUS_EXPR;
      break;
    case CPP_MULT:
      reduc_code = MULT_EXPR;
      break;
    case CPP_MINUS:
      reduc_code = MINUS_EXPR;
      break;
    case CPP_AND:
      reduc_code = BIT_AND_EXPR;
      break;
    case CPP_XOR:
      reduc_code = BIT_XOR_EXPR;
      break;
    case CPP_OR:
      reduc_code = BIT_IOR_EXPR;
      break;
    case CPP_AND_AND:
      reduc_code = TRUTH_ANDIF_EXPR;
      break;
    case CPP_OR_OR:
      reduc_code = TRUTH_ORIF_EXPR;
      break;
    case CPP_NAME:
      const char *p;
      p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp (p, "min") == 0)
	{
	  reduc_code = MIN_EXPR;
	  break;
	}
      if (strcmp (p, "max") == 0)
	{
	  reduc_code = MAX_EXPR;
	  break;
	}
      reduc_id = c_parser_peek_token (parser)->value;
      break;
    default:
      c_parser_error (parser,
		      "expected %<+%>, %<*%>, %<-%>, %<&%>, "
		      "%<^%>, %<|%>, %<&&%>, %<||%>, %<min%> or identifier");
      goto fail;
    }

  tree orig_reduc_id, reduc_decl;
  orig_reduc_id = reduc_id;
  reduc_id = c_omp_reduction_id (reduc_code, reduc_id);
  reduc_decl = c_omp_reduction_decl (reduc_id);
  c_parser_consume_token (parser);

  if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
    goto fail;

  while (true)
    {
      location_t loc = c_parser_peek_token (parser)->location;
      struct c_type_name *ctype = c_parser_type_name (parser);
      if (ctype != NULL)
	{
	  type = groktypename (ctype, NULL, NULL);
	  if (type == error_mark_node)
	    ;
	  else if ((INTEGRAL_TYPE_P (type)
		    || TREE_CODE (type) == REAL_TYPE
		    || TREE_CODE (type) == COMPLEX_TYPE)
		   && orig_reduc_id == NULL_TREE)
	    error_at (loc, "predeclared arithmetic type in "
			   "%<#pragma omp declare reduction%>");
	  else if (TREE_CODE (type) == FUNCTION_TYPE
		   || TREE_CODE (type) == ARRAY_TYPE)
	    error_at (loc, "function or array type in "
		      "%<#pragma omp declare reduction%>");
	  else if (TYPE_QUALS_NO_ADDR_SPACE (type))
	    error_at (loc, "const, volatile or restrict qualified type in "
			   "%<#pragma omp declare reduction%>");
	  else
	    {
	      tree t;
	      for (t = DECL_INITIAL (reduc_decl); t; t = TREE_CHAIN (t))
		if (comptypes (TREE_PURPOSE (t), type))
		  {
		    error_at (loc, "redeclaration of %qs "
				   "%<#pragma omp declare reduction%> for "
				   "type %qT",
				   IDENTIFIER_POINTER (reduc_id)
				   + sizeof ("omp declare reduction ") - 1,
				   type);
		    location_t ploc
		      = DECL_SOURCE_LOCATION (TREE_VEC_ELT (TREE_VALUE (t),
							    0));
		    error_at (ploc, "previous %<#pragma omp declare "
				    "reduction%>");
		    break;
		  }
	      if (t == NULL_TREE)
		types.safe_push (type);
	    }
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    c_parser_consume_token (parser);
	  else
	    break;
	}
      else
	break;
    }

  if (!c_parser_require (parser, CPP_COLON, "expected %<:%>")
      || types.is_empty ())
    {
     fail:
      clauses.release ();
      types.release ();
      while (true)
	{
	  c_token *token = c_parser_peek_token (parser);
	  if (token->type == CPP_EOF || token->type == CPP_PRAGMA_EOL)
	    break;
	  c_parser_consume_token (parser);
	}
      c_parser_skip_to_pragma_eol (parser);
      return;
    }

  if (types.length () > 1)
    {
      while (c_parser_next_token_is_not (parser, CPP_PRAGMA_EOL))
	{
	  c_token *token = c_parser_peek_token (parser);
	  if (token->type == CPP_EOF)
	    goto fail;
	  clauses.safe_push (*token);
	  c_parser_consume_token (parser);
	}
      clauses.safe_push (*c_parser_peek_token (parser));
      c_parser_skip_to_pragma_eol (parser);

      /* Make sure nothing tries to read past the end of the tokens.  */
      c_token eof_token;
      memset (&eof_token, 0, sizeof (eof_token));
      eof_token.type = CPP_EOF;
      clauses.safe_push (eof_token);
      clauses.safe_push (eof_token);
    }

  int errs = errorcount;
  FOR_EACH_VEC_ELT (types, i, type)
    {
      tokens_avail = parser->tokens_avail;
      gcc_assert (parser->tokens == &parser->tokens_buf[0]);
      if (!clauses.is_empty ())
	{
	  parser->tokens = clauses.address ();
	  parser->tokens_avail = clauses.length ();
	  parser->in_pragma = true;
	}

      bool nested = current_function_decl != NULL_TREE;
      if (nested)
	c_push_function_context ();
      tree fndecl = build_decl (BUILTINS_LOCATION, FUNCTION_DECL,
				reduc_id, default_function_type);
      current_function_decl = fndecl;
      allocate_struct_function (fndecl, true);
      push_scope ();
      tree stmt = push_stmt_list ();
      /* Intentionally BUILTINS_LOCATION, so that -Wshadow doesn't
	 warn about these.  */
      tree omp_out = build_decl (BUILTINS_LOCATION, VAR_DECL,
				 get_identifier ("omp_out"), type);
      DECL_ARTIFICIAL (omp_out) = 1;
      DECL_CONTEXT (omp_out) = fndecl;
      pushdecl (omp_out);
      tree omp_in = build_decl (BUILTINS_LOCATION, VAR_DECL,
				get_identifier ("omp_in"), type);
      DECL_ARTIFICIAL (omp_in) = 1;
      DECL_CONTEXT (omp_in) = fndecl;
      pushdecl (omp_in);
      struct c_expr combiner = c_parser_expression (parser);
      struct c_expr initializer;
      tree omp_priv = NULL_TREE, omp_orig = NULL_TREE;
      bool bad = false;
      initializer.value = error_mark_node;
      if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
	bad = true;
      else if (c_parser_next_token_is (parser, CPP_NAME)
	       && strcmp (IDENTIFIER_POINTER
				(c_parser_peek_token (parser)->value),
			  "initializer") == 0)
	{
	  c_parser_consume_token (parser);
	  pop_scope ();
	  push_scope ();
	  omp_priv = build_decl (BUILTINS_LOCATION, VAR_DECL,
				 get_identifier ("omp_priv"), type);
	  DECL_ARTIFICIAL (omp_priv) = 1;
	  DECL_INITIAL (omp_priv) = error_mark_node;
	  DECL_CONTEXT (omp_priv) = fndecl;
	  pushdecl (omp_priv);
	  omp_orig = build_decl (BUILTINS_LOCATION, VAR_DECL,
				 get_identifier ("omp_orig"), type);
	  DECL_ARTIFICIAL (omp_orig) = 1;
	  DECL_CONTEXT (omp_orig) = fndecl;
	  pushdecl (omp_orig);
	  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	    bad = true;
	  else if (!c_parser_next_token_is (parser, CPP_NAME))
	    {
	      c_parser_error (parser, "expected %<omp_priv%> or "
				      "function-name");
	      bad = true;
	    }
	  else if (strcmp (IDENTIFIER_POINTER
				(c_parser_peek_token (parser)->value),
			   "omp_priv") != 0)
	    {
	      if (c_parser_peek_2nd_token (parser)->type != CPP_OPEN_PAREN
		  || c_parser_peek_token (parser)->id_kind != C_ID_ID)
		{
		  c_parser_error (parser, "expected function-name %<(%>");
		  bad = true;
		}
	      else
		initializer = c_parser_postfix_expression (parser);
	      if (initializer.value
		  && TREE_CODE (initializer.value) == CALL_EXPR)
		{
		  int j;
		  tree c = initializer.value;
		  for (j = 0; j < call_expr_nargs (c); j++)
		    if (TREE_CODE (CALL_EXPR_ARG (c, j)) == ADDR_EXPR
			&& TREE_OPERAND (CALL_EXPR_ARG (c, j), 0) == omp_priv)
		      break;
		  if (j == call_expr_nargs (c))
		    error ("one of the initializer call arguments should be "
			   "%<&omp_priv%>");
		}
	    }
	  else
	    {
	      c_parser_consume_token (parser);
	      if (!c_parser_require (parser, CPP_EQ, "expected %<=%>"))
		bad = true;
	      else
		{
		  tree st = push_stmt_list ();
		  start_init (omp_priv, NULL_TREE, 0);
		  location_t loc = c_parser_peek_token (parser)->location;
		  struct c_expr init = c_parser_initializer (parser);
		  finish_init ();
		  finish_decl (omp_priv, loc, init.value,
		      	       init.original_type, NULL_TREE);
		  pop_stmt_list (st);
		}
	    }
	  if (!bad
	      && !c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
	    bad = true;
	}

      if (!bad)
	{
	  c_parser_skip_to_pragma_eol (parser);

	  tree t = tree_cons (type, make_tree_vec (omp_priv ? 6 : 3),
			      DECL_INITIAL (reduc_decl));
	  DECL_INITIAL (reduc_decl) = t;
	  DECL_SOURCE_LOCATION (omp_out) = rloc;
	  TREE_VEC_ELT (TREE_VALUE (t), 0) = omp_out;
	  TREE_VEC_ELT (TREE_VALUE (t), 1) = omp_in;
	  TREE_VEC_ELT (TREE_VALUE (t), 2) = combiner.value;
	  walk_tree (&combiner.value, c_check_omp_declare_reduction_r,
		     &TREE_VEC_ELT (TREE_VALUE (t), 0), NULL);
	  if (omp_priv)
	    {
	      DECL_SOURCE_LOCATION (omp_priv) = rloc;
	      TREE_VEC_ELT (TREE_VALUE (t), 3) = omp_priv;
	      TREE_VEC_ELT (TREE_VALUE (t), 4) = omp_orig;
	      TREE_VEC_ELT (TREE_VALUE (t), 5) = initializer.value;
	      walk_tree (&initializer.value, c_check_omp_declare_reduction_r,
			 &TREE_VEC_ELT (TREE_VALUE (t), 3), NULL);
	      walk_tree (&DECL_INITIAL (omp_priv),
			 c_check_omp_declare_reduction_r,
			 &TREE_VEC_ELT (TREE_VALUE (t), 3), NULL);
	    }
	}

      pop_stmt_list (stmt);
      pop_scope ();
      if (cfun->language != NULL)
	{
	  ggc_free (cfun->language);
	  cfun->language = NULL;
	}
      set_cfun (NULL);
      current_function_decl = NULL_TREE;
      if (nested)
	c_pop_function_context ();

      if (!clauses.is_empty ())
	{
	  parser->tokens = &parser->tokens_buf[0];
	  parser->tokens_avail = tokens_avail;
	}
      if (bad)
	goto fail;
      if (errs != errorcount)
	break;
    }

  clauses.release ();
  types.release ();
}


/* OpenMP 4.0
   #pragma omp declare simd declare-simd-clauses[optseq] new-line
   #pragma omp declare reduction (reduction-id : typename-list : expression) \
      initializer-clause[opt] new-line
   #pragma omp declare target new-line  */

static void
c_parser_omp_declare (c_parser *parser, enum pragma_context context)
{
  c_parser_consume_pragma (parser);
  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      const char *p = IDENTIFIER_POINTER (c_parser_peek_token (parser)->value);
      if (strcmp (p, "simd") == 0)
	{
	  /* c_parser_consume_token (parser); done in
	     c_parser_omp_declare_simd.  */
	  c_parser_omp_declare_simd (parser, context);
	  return;
	}
      if (strcmp (p, "reduction") == 0)
	{
	  c_parser_consume_token (parser);
	  c_parser_omp_declare_reduction (parser, context);
	  return;
	}
      if (!flag_openmp)  /* flag_openmp_simd  */
	{
	  c_parser_skip_to_pragma_eol (parser);
	  return;
	}
      if (strcmp (p, "target") == 0)
	{
	  c_parser_consume_token (parser);
	  c_parser_omp_declare_target (parser);
	  return;
	}
    }

  c_parser_error (parser, "expected %<simd%> or %<reduction%> "
			  "or %<target%>");
  c_parser_skip_to_pragma_eol (parser);
}

/* Main entry point to parsing most OpenMP pragmas.  */

static void
c_parser_omp_construct (c_parser *parser)
{
  enum pragma_kind p_kind;
  location_t loc;
  tree stmt;
  char p_name[sizeof "#pragma omp teams distribute parallel for simd"];
  omp_clause_mask mask (0);

  loc = c_parser_peek_token (parser)->location;
  p_kind = c_parser_peek_token (parser)->pragma_kind;
  c_parser_consume_pragma (parser);

  switch (p_kind)
    {
    case PRAGMA_OMP_ATOMIC:
      c_parser_omp_atomic (loc, parser);
      return;
    case PRAGMA_OMP_CRITICAL:
      stmt = c_parser_omp_critical (loc, parser);
      break;
    case PRAGMA_OMP_DISTRIBUTE:
      strcpy (p_name, "#pragma omp");
      stmt = c_parser_omp_distribute (loc, parser, p_name, mask, NULL);
      break;
    case PRAGMA_OMP_FOR:
      strcpy (p_name, "#pragma omp");
      stmt = c_parser_omp_for (loc, parser, p_name, mask, NULL);
      break;
    case PRAGMA_OMP_MASTER:
      stmt = c_parser_omp_master (loc, parser);
      break;
    case PRAGMA_OMP_ORDERED:
      stmt = c_parser_omp_ordered (loc, parser);
      break;
    case PRAGMA_OMP_PARALLEL:
      strcpy (p_name, "#pragma omp");
      stmt = c_parser_omp_parallel (loc, parser, p_name, mask, NULL);
      break;
    case PRAGMA_OMP_SECTIONS:
      strcpy (p_name, "#pragma omp");
      stmt = c_parser_omp_sections (loc, parser, p_name, mask, NULL);
      break;
    case PRAGMA_OMP_SIMD:
      strcpy (p_name, "#pragma omp");
      stmt = c_parser_omp_simd (loc, parser, p_name, mask, NULL);
      break;
    case PRAGMA_OMP_SINGLE:
      stmt = c_parser_omp_single (loc, parser);
      break;
    case PRAGMA_OMP_TASK:
      stmt = c_parser_omp_task (loc, parser);
      break;
    case PRAGMA_OMP_TASKGROUP:
      stmt = c_parser_omp_taskgroup (parser);
      break;
    case PRAGMA_OMP_TEAMS:
      strcpy (p_name, "#pragma omp");
      stmt = c_parser_omp_teams (loc, parser, p_name, mask, NULL);
      break;
    default:
      gcc_unreachable ();
    }

  if (stmt)
    gcc_assert (EXPR_LOCATION (stmt) != UNKNOWN_LOCATION);
}


/* OpenMP 2.5:
   # pragma omp threadprivate (variable-list) */

static void
c_parser_omp_threadprivate (c_parser *parser)
{
  tree vars, t;
  location_t loc;

  c_parser_consume_pragma (parser);
  loc = c_parser_peek_token (parser)->location;
  vars = c_parser_omp_var_list_parens (parser, OMP_CLAUSE_ERROR, NULL);

  /* Mark every variable in VARS to be assigned thread local storage.  */
  for (t = vars; t; t = TREE_CHAIN (t))
    {
      tree v = TREE_PURPOSE (t);

      /* FIXME diagnostics: Ideally we should keep individual
	 locations for all the variables in the var list to make the
	 following errors more precise.  Perhaps
	 c_parser_omp_var_list_parens() should construct a list of
	 locations to go along with the var list.  */

      /* If V had already been marked threadprivate, it doesn't matter
	 whether it had been used prior to this point.  */
      if (TREE_CODE (v) != VAR_DECL)
	error_at (loc, "%qD is not a variable", v);
      else if (TREE_USED (v) && !C_DECL_THREADPRIVATE_P (v))
	error_at (loc, "%qE declared %<threadprivate%> after first use", v);
      else if (! TREE_STATIC (v) && ! DECL_EXTERNAL (v))
	error_at (loc, "automatic variable %qE cannot be %<threadprivate%>", v);
      else if (TREE_TYPE (v) == error_mark_node)
	;
      else if (! COMPLETE_TYPE_P (TREE_TYPE (v)))
	error_at (loc, "%<threadprivate%> %qE has incomplete type", v);
      else
	{
	  if (! DECL_THREAD_LOCAL_P (v))
	    {
	      set_decl_tls_model (v, decl_default_tls_model (v));
	      /* If rtl has been already set for this var, call
		 make_decl_rtl once again, so that encode_section_info
		 has a chance to look at the new decl flags.  */
	      if (DECL_RTL_SET_P (v))
		make_decl_rtl (v);
	    }
	  C_DECL_THREADPRIVATE_P (v) = 1;
	}
    }

  c_parser_skip_to_pragma_eol (parser);
}

/* Cilk Plus <#pragma simd> parsing routines.  */

/* Helper function for c_parser_pragma.  Perform some sanity checking
   for <#pragma simd> constructs.  Returns FALSE if there was a
   problem.  */

static bool
c_parser_cilk_verify_simd (c_parser *parser,
				  enum pragma_context context)
{
  if (!flag_cilkplus)
    {
      warning (0, "pragma simd ignored because -fcilkplus is not enabled");
      c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
      return false;
    }
  if (context == pragma_external)
    {
      c_parser_error (parser,"pragma simd must be inside a function");
      c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
      return false;
    }
  return true;
}

/* Cilk Plus:
   This function is shared by SIMD-enabled functions and #pragma simd.  
   If IS_SIMD_FN is true then it is parsing a SIMD-enabled function and 
   CLAUSES is unused.  The main purpose of this function is to parse a
   vectorlength attribute or clause and check for parse errors.
   When IS_SIMD_FN is true then the function is merely caching the tokens
   in PARSER->CILK_SIMD_FN_TOKENS.  If errors are found then the token
   cache is cleared since there is no reason to continue.
   Syntax:
   vectorlength ( constant-expression )  */

static tree
c_parser_cilk_clause_vectorlength (c_parser *parser, tree clauses, 
				   bool is_simd_fn)
{
  if (is_simd_fn)
    check_no_duplicate_clause (clauses, OMP_CLAUSE_SIMDLEN, "vectorlength");
  else
  /* The vectorlength clause behaves exactly like OpenMP's safelen
     clause.  Represent it in OpenMP terms.  */
    check_no_duplicate_clause (clauses, OMP_CLAUSE_SAFELEN, "vectorlength");

  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return clauses;

  location_t loc = c_parser_peek_token (parser)->location;
  tree expr = c_parser_expr_no_commas (parser, NULL).value;
  expr = c_fully_fold (expr, false, NULL);

  /* If expr is an error_mark_node then the above function would have
     emitted an error.  No reason to do it twice.  */
  if (expr == error_mark_node)
    ;
  else if (!TREE_TYPE (expr)
	   || !TREE_CONSTANT (expr)
	   || !INTEGRAL_TYPE_P (TREE_TYPE (expr)))
  
    error_at (loc, "vectorlength must be an integer constant");  
  else if (wi::exact_log2 (expr) == -1)
    error_at (loc, "vectorlength must be a power of 2");
  else
    {
      if (is_simd_fn)
	{
	  tree u = build_omp_clause (loc, OMP_CLAUSE_SIMDLEN);
	  OMP_CLAUSE_SIMDLEN_EXPR (u) = expr;
	  OMP_CLAUSE_CHAIN (u) = clauses;
	  clauses = u;
	}
      else
	{
	  tree u = build_omp_clause (loc, OMP_CLAUSE_SAFELEN);
	  OMP_CLAUSE_SAFELEN_EXPR (u) = expr;
	  OMP_CLAUSE_CHAIN (u) = clauses;
	  clauses = u;
	}
    }

  c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>");

  return clauses;
}

/* Cilk Plus:
   linear ( simd-linear-variable-list )

   simd-linear-variable-list:
     simd-linear-variable
     simd-linear-variable-list , simd-linear-variable

   simd-linear-variable:
     id-expression
     id-expression : simd-linear-step

   simd-linear-step:
   conditional-expression */

static tree
c_parser_cilk_clause_linear (c_parser *parser, tree clauses)
{
  if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    return clauses;

  location_t loc = c_parser_peek_token (parser)->location;

  if (c_parser_next_token_is_not (parser, CPP_NAME)
      || c_parser_peek_token (parser)->id_kind != C_ID_ID)
    c_parser_error (parser, "expected identifier");

  while (c_parser_next_token_is (parser, CPP_NAME)
	 && c_parser_peek_token (parser)->id_kind == C_ID_ID)
    {
      tree var = lookup_name (c_parser_peek_token (parser)->value);

      if (var == NULL)
	{
	  undeclared_variable (c_parser_peek_token (parser)->location,
			       c_parser_peek_token (parser)->value);
	c_parser_consume_token (parser);
	}
      else if (var == error_mark_node)
	c_parser_consume_token (parser);
      else
	{
	  tree step = integer_one_node;

	  /* Parse the linear step if present.  */
	  if (c_parser_peek_2nd_token (parser)->type == CPP_COLON)
	    {
	      c_parser_consume_token (parser);
	      c_parser_consume_token (parser);

	      tree expr = c_parser_expr_no_commas (parser, NULL).value;
	      expr = c_fully_fold (expr, false, NULL);

	      if (TREE_TYPE (expr)
		  && INTEGRAL_TYPE_P (TREE_TYPE (expr))
		  && (TREE_CONSTANT (expr)
		      || DECL_P (expr)))
		step = expr;
	      else
		c_parser_error (parser,
				"step size must be an integer constant "
				"expression or an integer variable");
	    }
	  else
	    c_parser_consume_token (parser);

	  /* Use OMP_CLAUSE_LINEAR, which has the same semantics.  */
	  tree u = build_omp_clause (loc, OMP_CLAUSE_LINEAR);
	  OMP_CLAUSE_DECL (u) = var;
	  OMP_CLAUSE_LINEAR_STEP (u) = step;
	  OMP_CLAUSE_CHAIN (u) = clauses;
	  clauses = u;
	}

      if (c_parser_next_token_is_not (parser, CPP_COMMA))
	break;

      c_parser_consume_token (parser);
    }

  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, "expected %<)%>");

  return clauses;
}

/* Returns the name of the next clause.  If the clause is not
   recognized SIMD_OMP_CLAUSE_NONE is returned and the next token is
   not consumed.  Otherwise, the appropriate pragma_simd_clause is
   returned and the token is consumed.  */

static pragma_omp_clause
c_parser_cilk_clause_name (c_parser *parser)
{
  pragma_omp_clause result;
  c_token *token = c_parser_peek_token (parser);

  if (!token->value || token->type != CPP_NAME)
    return PRAGMA_CILK_CLAUSE_NONE;

  const char *p = IDENTIFIER_POINTER (token->value);

  if (!strcmp (p, "vectorlength"))
    result = PRAGMA_CILK_CLAUSE_VECTORLENGTH;
  else if (!strcmp (p, "linear"))
    result = PRAGMA_CILK_CLAUSE_LINEAR;
  else if (!strcmp (p, "private"))
    result = PRAGMA_CILK_CLAUSE_PRIVATE;
  else if (!strcmp (p, "firstprivate"))
    result = PRAGMA_CILK_CLAUSE_FIRSTPRIVATE;
  else if (!strcmp (p, "lastprivate"))
    result = PRAGMA_CILK_CLAUSE_LASTPRIVATE;
  else if (!strcmp (p, "reduction"))
    result = PRAGMA_CILK_CLAUSE_REDUCTION;
  else
    return PRAGMA_CILK_CLAUSE_NONE;

  c_parser_consume_token (parser);
  return result;
}

/* Parse all #<pragma simd> clauses.  Return the list of clauses
   found.  */

static tree
c_parser_cilk_all_clauses (c_parser *parser)
{
  tree clauses = NULL;

  while (c_parser_next_token_is_not (parser, CPP_PRAGMA_EOL))
    {
      pragma_omp_clause c_kind;

      c_kind = c_parser_cilk_clause_name (parser);

      switch (c_kind)
	{
	case PRAGMA_CILK_CLAUSE_VECTORLENGTH:
	  clauses = c_parser_cilk_clause_vectorlength (parser, clauses, false);
	  break;
	case PRAGMA_CILK_CLAUSE_LINEAR:
	  clauses = c_parser_cilk_clause_linear (parser, clauses);
	  break;
	case PRAGMA_CILK_CLAUSE_PRIVATE:
	  /* Use the OpenMP counterpart.  */
	  clauses = c_parser_omp_clause_private (parser, clauses);
	  break;
	case PRAGMA_CILK_CLAUSE_FIRSTPRIVATE:
	  /* Use the OpenMP counterpart.  */
	  clauses = c_parser_omp_clause_firstprivate (parser, clauses);
	  break;
	case PRAGMA_CILK_CLAUSE_LASTPRIVATE:
	  /* Use the OpenMP counterpart.  */
	  clauses = c_parser_omp_clause_lastprivate (parser, clauses);
	  break;
	case PRAGMA_CILK_CLAUSE_REDUCTION:
	  /* Use the OpenMP counterpart.  */
	  clauses = c_parser_omp_clause_reduction (parser, clauses);
	  break;
	default:
	  c_parser_error (parser, "expected %<#pragma simd%> clause");
	  goto saw_error;
	}
    }

 saw_error:
  c_parser_skip_to_pragma_eol (parser);
  return c_finish_cilk_clauses (clauses);
}

/* This function helps parse the grainsize pragma for a _Cilk_for statement.
   Here is the correct syntax of this pragma:
	    #pragma cilk grainsize = <EXP>
 */

static void
c_parser_cilk_grainsize (c_parser *parser)
{
  extern tree convert_to_integer (tree, tree);

  /* consume the 'grainsize' keyword.  */
  c_parser_consume_pragma (parser);

  if (c_parser_require (parser, CPP_EQ, "expected %<=%>") != 0)
    {
      struct c_expr g_expr = c_parser_binary_expression (parser, NULL, NULL);
      if (g_expr.value == error_mark_node)
	{
	  c_parser_skip_to_pragma_eol (parser);
	  return;
	}
      tree grain = convert_to_integer (long_integer_type_node,
				       c_fully_fold (g_expr.value, false,
						     NULL));
      c_parser_skip_to_pragma_eol (parser);
      c_token *token = c_parser_peek_token (parser);
      if (token && token->type == CPP_KEYWORD
	  && token->keyword == RID_CILK_FOR)
	{
	  if (grain == NULL_TREE || grain == error_mark_node)
	    grain = integer_zero_node;
	  c_parser_cilk_for (parser, grain);
	}
      else
	warning (0, "%<#pragma cilk grainsize%> is not followed by "
		    "%<_Cilk_for%>");
    }
  else
    c_parser_skip_to_pragma_eol (parser);
}

/* Main entry point for parsing Cilk Plus <#pragma simd> for loops.  */

static void
c_parser_cilk_simd (c_parser *parser)
{
  tree clauses = c_parser_cilk_all_clauses (parser);
  tree block = c_begin_compound_stmt (true);
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_omp_for_loop (loc, parser, CILK_SIMD, clauses, NULL);
  block = c_end_compound_stmt (loc, block, true);
  add_stmt (block);
}

/* Create an artificial decl with TYPE and emit initialization of it with
   INIT.  */

static tree
c_get_temp_regvar (tree type, tree init)
{
  location_t loc = EXPR_LOCATION (init);
  tree decl = build_decl (loc, VAR_DECL, NULL_TREE, type);
  DECL_ARTIFICIAL (decl) = 1;
  DECL_IGNORED_P (decl) = 1;
  pushdecl (decl);
  tree t = build2 (INIT_EXPR, type, decl, init);
  add_stmt (t);
  return decl;
}

/* Main entry point for parsing Cilk Plus _Cilk_for loops.
  GRAIN is the grain value passed in through pragma or 0.  */

static void
c_parser_cilk_for (c_parser *parser, tree grain)
{
  tree clauses = build_omp_clause (EXPR_LOCATION (grain), OMP_CLAUSE_SCHEDULE);
  OMP_CLAUSE_SCHEDULE_KIND (clauses) = OMP_CLAUSE_SCHEDULE_CILKFOR;
  OMP_CLAUSE_SCHEDULE_CHUNK_EXPR (clauses) = grain;
  clauses = c_finish_omp_clauses (clauses);

  tree block = c_begin_compound_stmt (true);
  tree sb = push_stmt_list ();
  location_t loc = c_parser_peek_token (parser)->location;
  tree omp_for = c_parser_omp_for_loop (loc, parser, CILK_FOR, clauses, NULL);
  sb = pop_stmt_list (sb);

  if (omp_for)
    {
      tree omp_par = make_node (OMP_PARALLEL);
      TREE_TYPE (omp_par) = void_type_node;
      OMP_PARALLEL_CLAUSES (omp_par) = NULL_TREE;
      tree bind = build3 (BIND_EXPR, void_type_node, NULL, NULL, NULL);
      TREE_SIDE_EFFECTS (bind) = 1;
      BIND_EXPR_BODY (bind) = sb;
      OMP_PARALLEL_BODY (omp_par) = bind;
      if (OMP_FOR_PRE_BODY (omp_for))
	{
	  add_stmt (OMP_FOR_PRE_BODY (omp_for));
	  OMP_FOR_PRE_BODY (omp_for) = NULL_TREE;
	}
      tree init = TREE_VEC_ELT (OMP_FOR_INIT (omp_for), 0);
      tree decl = TREE_OPERAND (init, 0);
      tree cond = TREE_VEC_ELT (OMP_FOR_COND (omp_for), 0);
      tree incr = TREE_VEC_ELT (OMP_FOR_INCR (omp_for), 0);
      tree t = TREE_OPERAND (cond, 1), c, clauses = NULL_TREE;
      if (TREE_CODE (t) != INTEGER_CST)
	{
	  TREE_OPERAND (cond, 1) = c_get_temp_regvar (TREE_TYPE (t), t);
	  c = build_omp_clause (input_location, OMP_CLAUSE_FIRSTPRIVATE);
	  OMP_CLAUSE_DECL (c) = TREE_OPERAND (cond, 1);
	  OMP_CLAUSE_CHAIN (c) = clauses;
	  clauses = c;
	}
      if (TREE_CODE (incr) == MODIFY_EXPR)
	{
	  t = TREE_OPERAND (TREE_OPERAND (incr, 1), 1);
	  if (TREE_CODE (t) != INTEGER_CST)
	    {
	      TREE_OPERAND (TREE_OPERAND (incr, 1), 1)
		= c_get_temp_regvar (TREE_TYPE (t), t);
	      c = build_omp_clause (input_location, OMP_CLAUSE_FIRSTPRIVATE);
	      OMP_CLAUSE_DECL (c) = TREE_OPERAND (TREE_OPERAND (incr, 1), 1);
	      OMP_CLAUSE_CHAIN (c) = clauses;
	      clauses = c;
	    }
	}
      t = TREE_OPERAND (init, 1);
      if (TREE_CODE (t) != INTEGER_CST)
	{
	  TREE_OPERAND (init, 1) = c_get_temp_regvar (TREE_TYPE (t), t);
	  c = build_omp_clause (input_location, OMP_CLAUSE_FIRSTPRIVATE);
	  OMP_CLAUSE_DECL (c) = TREE_OPERAND (init, 1);
	  OMP_CLAUSE_CHAIN (c) = clauses;
	  clauses = c;
	}
      c = build_omp_clause (input_location, OMP_CLAUSE_PRIVATE);
      OMP_CLAUSE_DECL (c) = decl;
      OMP_CLAUSE_CHAIN (c) = clauses;
      clauses = c;
      c = build_omp_clause (input_location, OMP_CLAUSE__CILK_FOR_COUNT_);
      OMP_CLAUSE_OPERAND (c, 0)
	= cilk_for_number_of_iterations (omp_for);
      OMP_CLAUSE_CHAIN (c) = clauses;
      OMP_PARALLEL_CLAUSES (omp_par) = c_finish_omp_clauses (c);
      add_stmt (omp_par);
    }

  block = c_end_compound_stmt (loc, block, true);
  add_stmt (block);
}


/* Parse a transaction attribute (GCC Extension).

   transaction-attribute:
     attributes
     [ [ any-word ] ]

   The transactional memory language description is written for C++,
   and uses the C++0x attribute syntax.  For compatibility, allow the
   bracket style for transactions in C as well.  */

static tree
c_parser_transaction_attributes (c_parser *parser)
{
  tree attr_name, attr = NULL;

  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
    return c_parser_attributes (parser);

  if (!c_parser_next_token_is (parser, CPP_OPEN_SQUARE))
    return NULL_TREE;
  c_parser_consume_token (parser);
  if (!c_parser_require (parser, CPP_OPEN_SQUARE, "expected %<[%>"))
    goto error1;

  attr_name = c_parser_attribute_any_word (parser);
  if (attr_name)
    {
      c_parser_consume_token (parser);
      attr = build_tree_list (attr_name, NULL_TREE);
    }
  else
    c_parser_error (parser, "expected identifier");

  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, "expected %<]%>");
 error1:
  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, "expected %<]%>");
  return attr;
}

/* Parse a __transaction_atomic or __transaction_relaxed statement
   (GCC Extension).

   transaction-statement:
     __transaction_atomic transaction-attribute[opt] compound-statement
     __transaction_relaxed compound-statement

   Note that the only valid attribute is: "outer".
*/

static tree
c_parser_transaction (c_parser *parser, enum rid keyword)
{
  unsigned int old_in = parser->in_transaction;
  unsigned int this_in = 1, new_in;
  location_t loc = c_parser_peek_token (parser)->location;
  tree stmt, attrs;

  gcc_assert ((keyword == RID_TRANSACTION_ATOMIC
      || keyword == RID_TRANSACTION_RELAXED)
      && c_parser_next_token_is_keyword (parser, keyword));
  c_parser_consume_token (parser);

  if (keyword == RID_TRANSACTION_RELAXED)
    this_in |= TM_STMT_ATTR_RELAXED;
  else
    {
      attrs = c_parser_transaction_attributes (parser);
      if (attrs)
	this_in |= parse_tm_stmt_attr (attrs, TM_STMT_ATTR_OUTER);
    }

  /* Keep track if we're in the lexical scope of an outer transaction.  */
  new_in = this_in | (old_in & TM_STMT_ATTR_OUTER);

  parser->in_transaction = new_in;
  stmt = c_parser_compound_statement (parser);
  parser->in_transaction = old_in;

  if (flag_tm)
    stmt = c_finish_transaction (loc, stmt, this_in);
  else
    error_at (loc, (keyword == RID_TRANSACTION_ATOMIC ?
	"%<__transaction_atomic%> without transactional memory support enabled"
	: "%<__transaction_relaxed %> "
	"without transactional memory support enabled"));

  return stmt;
}

/* Parse a __transaction_atomic or __transaction_relaxed expression
   (GCC Extension).

   transaction-expression:
     __transaction_atomic ( expression )
     __transaction_relaxed ( expression )
*/

static struct c_expr
c_parser_transaction_expression (c_parser *parser, enum rid keyword)
{
  struct c_expr ret;
  unsigned int old_in = parser->in_transaction;
  unsigned int this_in = 1;
  location_t loc = c_parser_peek_token (parser)->location;
  tree attrs;

  gcc_assert ((keyword == RID_TRANSACTION_ATOMIC
      || keyword == RID_TRANSACTION_RELAXED)
      && c_parser_next_token_is_keyword (parser, keyword));
  c_parser_consume_token (parser);

  if (keyword == RID_TRANSACTION_RELAXED)
    this_in |= TM_STMT_ATTR_RELAXED;
  else
    {
      attrs = c_parser_transaction_attributes (parser);
      if (attrs)
	this_in |= parse_tm_stmt_attr (attrs, 0);
    }

  parser->in_transaction = this_in;
  if (c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
    {
      tree expr = c_parser_expression (parser).value;
      ret.original_type = TREE_TYPE (expr);
      ret.value = build1 (TRANSACTION_EXPR, ret.original_type, expr);
      if (this_in & TM_STMT_ATTR_RELAXED)
	TRANSACTION_EXPR_RELAXED (ret.value) = 1;
      SET_EXPR_LOCATION (ret.value, loc);
      ret.original_code = TRANSACTION_EXPR;
      if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  goto error;
	}
    }
  else
    {
     error:
      ret.value = error_mark_node;
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
    }
  parser->in_transaction = old_in;

  if (!flag_tm)
    error_at (loc, (keyword == RID_TRANSACTION_ATOMIC ?
	"%<__transaction_atomic%> without transactional memory support enabled"
	: "%<__transaction_relaxed %> "
	"without transactional memory support enabled"));

  return ret;
}

/* Parse a __transaction_cancel statement (GCC Extension).

   transaction-cancel-statement:
     __transaction_cancel transaction-attribute[opt] ;

   Note that the only valid attribute is "outer".
*/

static tree
c_parser_transaction_cancel (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  tree attrs;
  bool is_outer = false;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_TRANSACTION_CANCEL));
  c_parser_consume_token (parser);

  attrs = c_parser_transaction_attributes (parser);
  if (attrs)
    is_outer = (parse_tm_stmt_attr (attrs, TM_STMT_ATTR_OUTER) != 0);

  if (!flag_tm)
    {
      error_at (loc, "%<__transaction_cancel%> without "
		"transactional memory support enabled");
      goto ret_error;
    }
  else if (parser->in_transaction & TM_STMT_ATTR_RELAXED)
    {
      error_at (loc, "%<__transaction_cancel%> within a "
		"%<__transaction_relaxed%>");
      goto ret_error;
    }
  else if (is_outer)
    {
      if ((parser->in_transaction & TM_STMT_ATTR_OUTER) == 0
	  && !is_tm_may_cancel_outer (current_function_decl))
	{
	  error_at (loc, "outer %<__transaction_cancel%> not "
		    "within outer %<__transaction_atomic%>");
	  error_at (loc, "  or a %<transaction_may_cancel_outer%> function");
	  goto ret_error;
	}
    }
  else if (parser->in_transaction == 0)
    {
      error_at (loc, "%<__transaction_cancel%> not within "
		"%<__transaction_atomic%>");
      goto ret_error;
    }

  return add_stmt (build_tm_abort_call (loc, is_outer));

 ret_error:
  return build1 (NOP_EXPR, void_type_node, error_mark_node);
}

/* Parse a single source file.  */

void
c_parse_file (void)
{
  /* Use local storage to begin.  If the first token is a pragma, parse it.
     If it is #pragma GCC pch_preprocess, then this will load a PCH file
     which will cause garbage collection.  */
  c_parser tparser;

  memset (&tparser, 0, sizeof tparser);
  tparser.tokens = &tparser.tokens_buf[0];
  the_parser = &tparser;

  if (c_parser_peek_token (&tparser)->pragma_kind == PRAGMA_GCC_PCH_PREPROCESS)
    c_parser_pragma_pch_preprocess (&tparser);

  the_parser = ggc_alloc<c_parser> ();
  *the_parser = tparser;
  if (tparser.tokens == &tparser.tokens_buf[0])
    the_parser->tokens = &the_parser->tokens_buf[0];

  /* Initialize EH, if we've been told to do so.  */
  if (flag_exceptions)
    using_eh_for_cleanups ();

  c_parser_translation_unit (the_parser);
  the_parser = NULL;
}

/* This function parses Cilk Plus array notation.  The starting index is
   passed in INITIAL_INDEX and the array name is passes in ARRAY_VALUE.  The
   return value of this function is a tree_node called VALUE_TREE of type
   ARRAY_NOTATION_REF.  */

static tree 
c_parser_array_notation (location_t loc, c_parser *parser, tree initial_index, 
			 tree array_value)
{
  c_token *token = NULL;
  tree start_index = NULL_TREE, end_index = NULL_TREE, stride = NULL_TREE;
  tree value_tree = NULL_TREE, type = NULL_TREE, array_type = NULL_TREE;
  tree array_type_domain = NULL_TREE; 

  if (array_value == error_mark_node || initial_index == error_mark_node)
    {
      /* No need to continue.  If either of these 2 were true, then an error
	 must be emitted already.  Thus, no need to emit them twice.  */
      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
      return error_mark_node;
    }
  
  array_type = TREE_TYPE (array_value);
  gcc_assert (array_type);
  if (TREE_CODE (array_type) != ARRAY_TYPE
      && TREE_CODE (array_type) != POINTER_TYPE)
    {
      error_at (loc, "base of array section must be pointer or array type");
      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
      return error_mark_node;
    }
  type = TREE_TYPE (array_type);
  token = c_parser_peek_token (parser);
   
  if (token->type == CPP_EOF)
    {
      c_parser_error (parser, "expected %<:%> or numeral");
      return value_tree;
    }
  else if (token->type == CPP_COLON)
    {
      if (!initial_index)
	{
	  /* If we are here, then we have a case like this A[:].  */
	  c_parser_consume_token (parser);
	  if (TREE_CODE (array_type) == POINTER_TYPE)
	    {
	      error_at (loc, "start-index and length fields necessary for "
			"using array notations in pointers");
	      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	      return error_mark_node;
	    }
	  if (TREE_CODE (array_type) == FUNCTION_TYPE)
	    {
	      error_at (loc, "array notations cannot be used with function "
			"type");
	      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	      return error_mark_node;
	    }
	  array_type_domain = TYPE_DOMAIN (array_type);

	  if (!array_type_domain)
	    {
	      error_at (loc, "start-index and length fields necessary for "
			"using array notations in dimensionless arrays");
	      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	      return error_mark_node;
	    }

	  start_index = TYPE_MINVAL (array_type_domain);
	  start_index = fold_build1 (CONVERT_EXPR, ptrdiff_type_node,
				     start_index);
	  if (!TYPE_MAXVAL (array_type_domain)
	      || !TREE_CONSTANT (TYPE_MAXVAL (array_type_domain)))
	    {
	      error_at (loc, "start-index and length fields necessary for "
			"using array notations in variable-length arrays");
	      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	      return error_mark_node;
	    }
	  end_index = TYPE_MAXVAL (array_type_domain);
	  end_index = fold_build2 (PLUS_EXPR, TREE_TYPE (end_index),
				   end_index, integer_one_node);
	  end_index = fold_build1 (CONVERT_EXPR, ptrdiff_type_node, end_index);
	  stride = build_int_cst (integer_type_node, 1);
	  stride = fold_build1 (CONVERT_EXPR, ptrdiff_type_node, stride);
	}
      else if (initial_index != error_mark_node)
	{
	  /* If we are here, then there should be 2 possibilities:
	     1. Array [EXPR : EXPR]
	     2. Array [EXPR : EXPR : EXPR]
	  */
	  start_index = initial_index;

	  if (TREE_CODE (array_type) == FUNCTION_TYPE)
	    {
	      error_at (loc, "array notations cannot be used with function "
			"type");
	      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	      return error_mark_node;
	    }
	  c_parser_consume_token (parser); /* consume the ':' */
	  struct c_expr ce = c_parser_expression (parser);
	  ce = convert_lvalue_to_rvalue (loc, ce, false, false);
	  end_index = ce.value;
	  if (!end_index || end_index == error_mark_node)
	    {
	      c_parser_skip_to_end_of_block_or_statement (parser);
	      return error_mark_node;
	    }
	  if (c_parser_peek_token (parser)->type == CPP_COLON)
	    {
	      c_parser_consume_token (parser);
	      ce = c_parser_expression (parser);
	      ce = convert_lvalue_to_rvalue (loc, ce, false, false);
	      stride = ce.value;
	      if (!stride || stride == error_mark_node)
		{
		  c_parser_skip_to_end_of_block_or_statement (parser);
		  return error_mark_node;
		}
	    }
	}
      else
	c_parser_error (parser, "expected array notation expression");
    }
  else
    c_parser_error (parser, "expected array notation expression");
  
  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, "expected %<]%>");

  value_tree = build_array_notation_ref (loc, array_value, start_index,
					 end_index, stride, type);
  if (value_tree != error_mark_node)
    SET_EXPR_LOCATION (value_tree, loc);
  return value_tree;
}

#include "gt-c-c-parser.h"
