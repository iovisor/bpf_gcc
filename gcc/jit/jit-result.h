/* Internals of libgccjit: implementation of gcc_jit_result
   Copyright (C) 2013-2015 Free Software Foundation, Inc.
   Contributed by David Malcolm <dmalcolm@redhat.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#ifndef JIT_RESULT_H
#define JIT_RESULT_H

namespace gcc {

namespace jit {

/* The result of JIT-compilation.  */
class result
{
public:
  result(void *dso_handle);

  virtual ~result();

  void *
  get_code (const char *funcname);

private:
  void *m_dso_handle;
};

} // namespace gcc::jit

} // namespace gcc

#endif /* JIT_RESULT_H */
