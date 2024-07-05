/*
   Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/


#include "opt_hints_parser.h"
#include "sql_error.h"
#include "mysqld_error.h"
#include "sql_class.h"

Parse_context::Parse_context(THD *thd, st_select_lex *select)
: thd(thd),
  mem_root(thd->mem_root),
  select(select)
{}


// This method is for debug purposes
bool Optimizer_hint_parser::parse_token_list(THD *thd)
{
  for ( ; ; m_look_ahead_token= get_token(m_cs))
  {
    char tmp[200];
    my_snprintf(tmp, sizeof(tmp), "TOKEN: %d %.*s",
               (int) m_look_ahead_token.id(),
               (int) m_look_ahead_token.length,
               m_look_ahead_token.str);
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_UNKNOWN_ERROR, tmp);
    if (m_look_ahead_token.id() == TokenID::tNULL ||
        m_look_ahead_token.id() == TokenID::tEOF)
      break;
  }
  return true; // Success
}


void Optimizer_hint_parser::push_warning_syntax_error(THD *thd)
{
  const char *msg= ER_THD(thd, ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR);
  ErrConvString txt(m_look_ahead_token.str, strlen(m_look_ahead_token.str),
                    thd->variables.character_set_client);
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_PARSE_ERROR, ER_THD(thd, ER_PARSE_ERROR),
                      msg, txt.ptr(), 1);
}


bool
Optimizer_hint_parser::
  Table_name_list_container::add(Optimizer_hint_parser *p,
                                 Table_name &&elem)
{
  Table_name *pe= (Table_name*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool
Optimizer_hint_parser::
  Hint_param_table_list_container::add(Optimizer_hint_parser *p,
                                       Hint_param_table &&elem)
{
  Hint_param_table *pe= (Hint_param_table*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool
Optimizer_hint_parser::
  Hint_param_index_list_container::add(Optimizer_hint_parser *p,
                                       Hint_param_index &&elem)
{
  Hint_param_index *pe= (Hint_param_index*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool
Optimizer_hint_parser::
  Hint_list_container::add(Optimizer_hint_parser *p,
                           Hint &&elem)
{
  Hint *pe= (Hint*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


/**
  Returns a pointer to Opt_hints_global object,
  creates Opt_hints object if not exist.

  @param pc   pointer to Parse_context object

  @return  pointer to Opt_hints object,
           NULL if failed to create the object
*/

static Opt_hints_global *get_global_hints(Parse_context *pc)
{
  LEX *lex= pc->thd->lex;

  if (!lex->opt_hints_global)
    lex->opt_hints_global= new Opt_hints_global(pc->thd->mem_root);
  if (lex->opt_hints_global)
    lex->opt_hints_global->set_resolved();
  return lex->opt_hints_global;
}


static Opt_hints_qb *get_qb_hints(Parse_context *pc)
{
  if (pc->select->opt_hints_qb)
    return pc->select->opt_hints_qb;

  Opt_hints_global *global_hints= get_global_hints(pc);
  if (global_hints == NULL)
    return NULL;

  Opt_hints_qb *qb= new Opt_hints_qb(global_hints, pc->thd->mem_root,
                                     pc->select->select_number);
  if (qb)
  {
    global_hints->register_child(qb);
    pc->select->opt_hints_qb= qb;
    qb->set_resolved();
  }
  return qb;
}

bool Optimizer_hint_parser::Table_level_hint::resolve(Parse_context *pc) const
{
  const Table_level_hint_type &table_level_hint_type=
      static_cast<const Table_level_hint_type&>(*this);

  switch (table_level_hint_type.id())
  {
  case TokenID::keyword_BNL:
     
     break;
  case TokenID::keyword_NO_BNL:
   
     break;
  case TokenID::keyword_BKA:
     
     break;
  case TokenID::keyword_NO_BKA:
   
     break;
  default:
     DBUG_ASSERT(0);
     return true;
  }
  return false;
}


bool Optimizer_hint_parser::Index_level_hint::resolve(Parse_context *pc) const
{
  const Index_level_hint_type &index_level_hint_type=
      static_cast<const Index_level_hint_type&>(*this);

  switch (index_level_hint_type.id())
  {
  case TokenID::keyword_NO_ICP:
     
     break;
  case TokenID::keyword_MRR:
   
     break;
  case TokenID::keyword_NO_MRR:
   
     break;
  case TokenID::keyword_NO_RANGE_OPTIMIZATION:
   
     break;
  default:
     DBUG_ASSERT(0);
     return true;
  }
  return false;
}


bool Optimizer_hint_parser::Qb_name_hint::resolve(Parse_context *pc) const
{
  // OLEGS: todo
  // const  &index_level_hint_type=
  //     static_cast<const Index_level_hint_type&>(*this);

  // switch (token.id())
  // {
  // case TokenID::keyword_NO_ICP:
     
  //    break;
  // case TokenID::keyword_MRR:
   
  //    break;
  // case TokenID::keyword_NO_MRR:
   
  //    break;
  // case TokenID::keyword_NO_RANGE_OPTIMIZATION:
   
  //    break;
  // default:
  //    DBUG_ASSERT(0);
  //    return true;
  // }
  return false;
}

bool
Optimizer_hint_parser::
  Hint_list::resolve(Parse_context *pc)
{
  if (!get_qb_hints(pc))
    return true;

  List_iterator_fast<Optimizer_hint_parser::Hint> li(*this);
  while(Optimizer_hint_parser::Hint *hint= li++)
  {
    if (const Table_level_hint &table_hint=
        static_cast<const Table_level_hint &>(*hint))
    {
      if (table_hint.resolve(pc))
        return true;
    }
    else if (const Index_level_hint &index_hint=
             static_cast<const Index_level_hint &>(*hint))
    {
      if (index_hint.resolve(pc))
        return true;
    }
    else if (const Qb_name_hint &qb_hint=
             static_cast<const Qb_name_hint &>(*hint))
    {
      if (qb_hint.resolve(pc))
        return true;
    }
  }
  // for (PT_hint **h= hints.begin(), **end= hints.end(); h < end; h++)
  // {
  //   if (*h != NULL && (*h)->contextualize(pc))
  //     return true;
  // }
  return false;
}
