"""
 * File Name: testing.py
 * Description:
 *
 * Creation Date: 05-05-2018
 * Last Modified: Sat 05 May 2018 02:49:35 PM PDT
 *
 * Author: Max Moulds 
 *
"""
#!/usr/bin/python
import string
import random
import os
import sys

NUM_TEST_FILES = 100
LEN_TEXT = 100

def test_by_making_files():
  for x in range(0,NUM_TEST_FILES):
    f = open('FILE'+ str(x), 'w+')
    print('writing to FILE',x,' ...', sep='')
    for y in range(0, LEN_TEXT):
      f.write(random.choice(string.ascii_lowercase))
  f.close()
  for x in range(0, NUM_TEST_FILES):
    print('removing FILE',x, '... ', sep='')
    os.remove("FILE" + str(x))

def main():
    test_by_making_files()
    print('DONE')

if __name__ == "__main__":
  print('-- testing C-LOOK/SSTF? - Hello ', sys.argv[0])
  main()
else:
  print('-- called by another module -- ', sys.argv[0])
  main()

__author__ = "Max Moulds"
__copyright__ = "Copyright 2018, Max Moulds"
__credits__ = ["Max Moulds"]
__license__ = "Copyrighted, Restricted, ehh..."
__version__ = "0.1"
__maintainer__ = "Max Moulds"
__email__ = "max@maxmoulds.org"
__status__ = "Testing"
