// { dg-options "-std=gnu++11" }
// { dg-do compile }

// 2010-06-08  Paolo Carlini  <paolo.carlini@oracle.com>
//
// Copyright (C) 2010-2015 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#include <type_traits>
#include <testsuite_tr1.h>

void test01()
{
  using std::has_trivial_copy_assign;
  using namespace __gnu_test;

  static_assert(test_property<has_trivial_copy_assign, int>(true), "");
  static_assert(test_property<has_trivial_copy_assign, float>(true), "");
  static_assert(test_property<has_trivial_copy_assign, EnumType>(true), "");
  static_assert(test_property<has_trivial_copy_assign, int*>(true), "");
  static_assert(test_property<has_trivial_copy_assign, int(*)(int)>(true), "");
  static_assert(test_property<has_trivial_copy_assign,
		int (ClassType::*)>(true), "");
  static_assert(test_property<has_trivial_copy_assign,
		int (ClassType::*) (int)>(true), "");
  static_assert(test_property<has_trivial_copy_assign, int[2]>(true), "");
  static_assert(test_property<has_trivial_copy_assign, float[][3]>(true), "");
  static_assert(test_property<has_trivial_copy_assign,
		EnumType[2][3][4]>(true), "");
  static_assert(test_property<has_trivial_copy_assign, int*[3]>(true), "");
  static_assert(test_property<has_trivial_copy_assign,
		int(*[][2])(int)>(true), "");
  static_assert(test_property<has_trivial_copy_assign,
		int (ClassType::*[2][3])>(true), "");
  static_assert(test_property<has_trivial_copy_assign,
		int (ClassType::*[][2][3]) (int)>(true), "");

  // Negative tests.
  static_assert(test_property<has_trivial_copy_assign, void>(false), "");
}
