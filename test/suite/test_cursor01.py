#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base03.py
# 	Cursor operations
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest

class test_cursor01(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_base03a'
    nentries = 10

    def config_string(self):
        """
        Return any additional configuration.
        This method may be overridden.
        """
        return ''

    def session_create(self, name, args):
        """
        session.create, but report errors more completely
        """
        try:
            self.session.create(name, args)
        except:
            print('**** ERROR in session.create("' + name + '","' + args + '") ***** ')
            raise

    def test_forward_iter(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        create_args = 'key_format=S,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)

        for i in range(0, self.nentries):
            cursor.set_key('key' + str(i))
            cursor.set_value('value' + str(i))
            cursor.insert()

        # Don't use the builtin 'for ... in cursor',
        # iterate using the basic API.

        # 1. Calling first() should place us on the first k/v pair.
        cursor.first()
        nextret = 0
        i = 0
        while nextret == 0:
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, ('key' + str(i)))
            self.assertEqual(value, ('value' + str(i)))
            i += 1
            nextret = cursor.next()

        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, self.nentries)

        # After an error, we can no longer access the key or value
        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)

        # 2. Setting reset() should place us just before first pair.
        cursor.reset()

        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)
            
        nextret = cursor.next()
        i = 0
        while nextret == 0:
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, ('key' + str(i)))
            self.assertEqual(value, ('value' + str(i)))
            i += 1
            nextret = cursor.next()

        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, self.nentries)
        cursor.close(None)

    def test_backward_iter(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        create_args = 'key_format=S,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)

        for i in range(0, self.nentries):
            cursor.set_key('key' + str(i))
            cursor.set_value('value' + str(i))
            cursor.insert()

        # Don't use the builtin 'for ... in cursor',
        # iterate using the basic API.

        # 1. Calling first() should place us on the first k/v pair.
        cursor.last()
        prevret = 0
        i = self.nentries - 1
        while prevret == 0:
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, ('key' + str(i)))
            self.assertEqual(value, ('value' + str(i)))
            i -= 1
            prevret = cursor.prev()

        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, -1)

        # After an error, we can no longer access the key or value
        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)

        # 2. Setting reset() should place us just before first pair.
        cursor.reset()

        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)
            
        prevret = cursor.prev()
        i = self.nentries - 1
        while prevret == 0:
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, ('key' + str(i)))
            self.assertEqual(value, ('value' + str(i)))
            i -= 1
            prevret = cursor.prev()

        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, -1)
        cursor.close(None)

if __name__ == '__main__':
    wttest.run()
