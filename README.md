# OASIS / THEOS Disk Utility

[![Codacy Badge](https://app.codacy.com/project/badge/Grade/01e6dc1425da4801a9ca361eda559ae7)](https://www.codacy.com/gh/hharte/oasis-utils/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=hharte/oasis-utils&amp;utm_campaign=Badge_Grade)
[![Coverity](https://scan.coverity.com/projects/24657/badge.svg)](https://scan.coverity.com/projects/hharte-oasis-utils)

OASIS was a popular operating system used for business in the late 1970’s through the 1980’s on Z80-based computers.  

`oasis` can list and extract files from OASIS disk images, preserving file dates.

The filename in OASIS can be up to eight characters, with an eight character extension.  Each file has an associated file type:


<table>
  <tr>
   <td>Description
   </td>
   <td>Metadata
   </td>
   <td>Output Filename
   </td>
  </tr>
  <tr>
   <td>Relocatable
   </td>
   <td>record length
   </td>
   <td>fname.ftype_R_&lt;metadata>
   </td>
  </tr>
  <tr>
   <td>Absolute
   </td>
   <td>load address
   </td>
   <td>fname.ftype_A_&lt;metadata>
   </td>
  </tr>
  <tr>
   <td>Sequential
   </td>
   <td>record length of longest record
   </td>
   <td>fname.ftype_S_&lt;metadata>
   </td>
  </tr>
  <tr>
   <td>Direct
   </td>
   <td>allocated record length
   </td>
   <td>fname.ftype_D_&lt;metadata>
   </td>
  </tr>
  <tr>
   <td>Indexed
   </td>
   <td>
   </td>
   <td>Not supported
   </td>
  </tr>
  <tr>
   <td>Keyed
   </td>
   <td>
   </td>
   <td>Not supported
   </td>
  </tr>
</table>


In addition to the file name and extension, the OASIS filesystem stores metadata about certain types of files.  For executable object code, the “load address” is stored in the metadata.  To preserve this important information, `nsdos` appends the load address to the output filename extension.


# Usage


```
OASIS File Utility (c) 2021 - Howard M. Harte
https://github.com/hharte/oasis-utils

usage is: oasis <filename.img> [command] [<filename>|<path>] [-q] [-a]
        <filename.img> OASIS Disk Image in .img format.
        [command]      LI - List files
                       EX - Extract files to <path>
        Flags:
              -q       Quiet: Don't list file details during extraction.
	      -a       ASCII: Convert line endings and truncate output file at EOF.

        If no command is given, LIst is assumed.
```



## To list files on an OASIS disk image:


```
$ oasis <filename.img>
```


Or


```
$ oasis <filename.img> li
```


For example:


```
$ oasis OASIS_Users_Group_Vol01.img
Label: VOL$1
77-1-13
112 directory entries
23K free
Fname--- Ftype--  --Date-- Time- -Recs Blks Format- -Sect Own SOw Other-
BLKFRI2  BASICOBJ 06/19/81 18:50   362   10 S    85    16   0   0     54 E
LEM      BASICOBJ 06/19/81 19:22   233    6 S   129    76   0   0     98 E
COMPILE  EXEC     02/29/81 17:09    45    2 S    74   764   0   0    769 E
PURGE    EXEC     12/24/80 15:23    82    2 S    57   556   0   0    562 E
PERFRMCE ARTICLE  02/15/82 18:24   172    9 S    75   476   0   0    509 E
HARDDISK ARTICLE  09/04/81 16:39    27    2 S    75   736   0   0    741 E
SERVICE  INFORMTN 06/15/81 16:49    66    4 S    76   772   0   0    784 E
ADVENTUR WORK     05/14/80 00:00   200   50 D   256   112   0   0
ADVENTUR COMMAND  05/14/80 00:00    98   25 R   256   564   0   0  20300 L
NEWS     LETTER1  06/11/82 21:25    77    4 S    70   836   0   0    849 E
CHASE    BASICOBJ 06/19/81 18:53   111    3 S    65   100   0   0    109 E
WUMPUS   BASICOBJ 06/19/81 19:15   301    8 S    78   396   0   0    424 E
REPEAT   EXEC     11/19/80 19:44    24    1 S    61   552   0   0    554 E
HEXAPAWN BASICCOM 07/28/80 17:15   176    5 S    61   708   0   0    727 E
YAHTZEE  BASICCOM 09/23/80 13:05   282    7 S   117   428   0   0    452 E
SOFTWARE GUIDE    08/29/81 17:33    69    4 S    71   512   0   0    524 E
RUN$     EXEC     08/10/81 16:21    26    1 S    67   744   0   0    746 E
VOL1     EXEC     06/11/82 19:20   416   13 S    73   312   0   0    362 E
ADVENTUR MEMO     04/10/80 00:00    60    3 S    62   384   0   0    394 E
GAMES    EXEC     05/27/81 16:20    47    2 S    66   788   0   0    792 E
POETRY   BASICOBJ 03/13/81 15:20   116    4 S    81   796   0   0    808 E
DLRADVCE ARTICLE  05/10/82 11:34   129    8 S    75   852   0   0    882 E
LOAN     BASICOBJ 06/19/81 19:08    73    2 S    81   528   0   0    533 E
LANDER   BASICOBJ 06/19/81 18:58   122    4 S    66   536   0   0    549 E
AMAZE    BASICOBJ 06/19/81 18:46   187    4 S    73   748   0   0    762 E
DEALER   COSTINFO 06/15/81 16:41    71    4 S    76   676   0   0    688 E
OASIS    HARDWARE 02/09/82 13:23    22    1 S    73   884   0   0    886 E
QUALITY  ARTICLE  05/10/82 11:37    54    3 S    75    64   0   0     75 E
S$TREK   BASICOBJ 05/08/81 18:19   211    5 S    56   456   0   0    473 E
LANGUAGE ARTICLE  06/15/81 16:37   100    5 S    76   364   0   0    383 E
SERVICE  ARTICLE  09/04/81 16:35    56    3 S    75   664   0   0    675 E
$README  FIRST1   03/22/82 00:15    77    4 S    75   692   0   0    705 E
EDTORIAL ARTICLE  02/15/82 18:17    68    4 S    75   888   0   0    902 E
BAGELS   BASICOBJ 07/21/81 16:25    72    2 S    65    56   0   0     62 E
SELECT1  EXEC     12/01/81 17:22    36    2 S    46   728   0   0    734 E
AWARI    BASICCOM 06/04/80 12:37   131    4 S    64   812   0   0    825 E
LOVE     BASICOBJ 06/19/81 19:11    36    2 S   113   828   0   0    834 E
COPYRITE NOTICE   05/13/82 18:33     8    1 S    65   904   0   0    905 E
```



## To Extract All Files from the disk image:


```
$ oasis <filename.img> ex <path>
```


For example:


```
$ oasis OASIS_Users_Group_Vol01.img ex OUG_Files
Label: VOL$1   
77-1-13
112 directory entries
23K free
BLKFRI2.BASICOBJ -> OUG_Files/BLKFRI2.BASICOBJ_S_85 (9906 bytes)
LEM.BASICOBJ -> OUG_Files/LEM.BASICOBJ_S_129 (5842 bytes)
COMPILE.EXEC -> OUG_Files/COMPILE.EXEC_S_74 (1524 bytes)
PURGE.EXEC -> OUG_Files/PURGE.EXEC_S_57 (1778 bytes)
PERFRMCE.ARTICLE -> OUG_Files/PERFRMCE.ARTICLE_S_75 (8636 bytes)
HARDDISK.ARTICLE -> OUG_Files/HARDDISK.ARTICLE_S_75 (1524 bytes)
SERVICE.INFORMTN -> OUG_Files/SERVICE.INFORMTN_S_76 (3302 bytes)
ADVENTUR.WORK -> OUG_Files/ADVENTUR.WORK_D_256 (51200 bytes)
ADVENTUR.COMMAND -> OUG_Files/ADVENTUR.COMMAND_R_256 (25088 bytes)
NEWS.LETTER1 -> OUG_Files/NEWS.LETTER1_S_70 (3556 bytes)
CHASE.BASICOBJ -> OUG_Files/CHASE.BASICOBJ_S_65 (2540 bytes)
WUMPUS.BASICOBJ -> OUG_Files/WUMPUS.BASICOBJ_S_78 (7366 bytes)
REPEAT.EXEC -> OUG_Files/REPEAT.EXEC_S_61 (762 bytes)
HEXAPAWN.BASICCOM -> OUG_Files/HEXAPAWN.BASICCOM_S_61 (5080 bytes)
YAHTZEE.BASICCOM -> OUG_Files/YAHTZEE.BASICCOM_S_117 (6350 bytes)
SOFTWARE.GUIDE -> OUG_Files/SOFTWARE.GUIDE_S_71 (3302 bytes)
RUN$.EXEC -> OUG_Files/RUN$.EXEC_S_67 (762 bytes)
VOL1.EXEC -> OUG_Files/VOL1.EXEC_S_73 (12954 bytes)
ADVENTUR.MEMO -> OUG_Files/ADVENTUR.MEMO_S_62 (2794 bytes)
GAMES.EXEC -> OUG_Files/GAMES.EXEC_S_66 (1270 bytes)
POETRY.BASICOBJ -> OUG_Files/POETRY.BASICOBJ_S_81 (3302 bytes)
DLRADVCE.ARTICLE -> OUG_Files/DLRADVCE.ARTICLE_S_75 (7874 bytes)
LOAN.BASICOBJ -> OUG_Files/LOAN.BASICOBJ_S_81 (1524 bytes)
LANDER.BASICOBJ -> OUG_Files/LANDER.BASICOBJ_S_66 (3556 bytes)
AMAZE.BASICOBJ -> OUG_Files/AMAZE.BASICOBJ_S_73 (3810 bytes)
DEALER.COSTINFO -> OUG_Files/DEALER.COSTINFO_S_76 (3302 bytes)
OASIS.HARDWARE -> OUG_Files/OASIS.HARDWARE_S_73 (762 bytes)
QUALITY.ARTICLE -> OUG_Files/QUALITY.ARTICLE_S_75 (3048 bytes)
S$TREK.BASICOBJ -> OUG_Files/S$TREK.BASICOBJ_S_56 (4572 bytes)
LANGUAGE.ARTICLE -> OUG_Files/LANGUAGE.ARTICLE_S_76 (5080 bytes)
SERVICE.ARTICLE -> OUG_Files/SERVICE.ARTICLE_S_75 (3048 bytes)
$README.FIRST1 -> OUG_Files/$README.FIRST1_S_75 (3556 bytes)
EDTORIAL.ARTICLE -> OUG_Files/EDTORIAL.ARTICLE_S_75 (3810 bytes)
BAGELS.BASICOBJ -> OUG_Files/BAGELS.BASICOBJ_S_65 (1778 bytes)
SELECT1.EXEC -> OUG_Files/SELECT1.EXEC_S_46 (1778 bytes)
AWARI.BASICCOM -> OUG_Files/AWARI.BASICCOM_S_64 (3556 bytes)
LOVE.BASICOBJ -> OUG_Files/LOVE.BASICOBJ_S_113 (1778 bytes)
COPYRITE.NOTICE -> OUG_Files/COPYRITE.NOTICE_S_65 (508 bytes)
Extracted 38 files.

