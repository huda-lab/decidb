/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton interface for Bison's Yacc-like parsers in C

   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     IDENT = 258,
     FCONST = 259,
     SCONST = 260,
     BCONST = 261,
     XCONST = 262,
     Op = 263,
     ICONST = 264,
     PARAM = 265,
     TYPECAST = 266,
     DOT_DOT = 267,
     COLON_EQUALS = 268,
     EQUALS_GREATER = 269,
     INTEGER_DIVISION = 270,
     POWER_OF = 271,
     LAMBDA_ARROW = 272,
     DOUBLE_ARROW = 273,
     LESS_EQUALS = 274,
     GREATER_EQUALS = 275,
     NOT_EQUALS = 276,
     ABORT_P = 277,
     ABSOLUTE_P = 278,
     ACCESS = 279,
     ACTION = 280,
     ADD_P = 281,
     ADMIN = 282,
     AFTER = 283,
     AGGREGATE = 284,
     ALL = 285,
     ALSO = 286,
     ALTER = 287,
     ALWAYS = 288,
     ANALYSE = 289,
     ANALYZE = 290,
     AND = 291,
     ANTI = 292,
     ANY = 293,
     ARRAY = 294,
     AS = 295,
     ASC_P = 296,
     ASOF = 297,
     ASSERTION = 298,
     ASSIGNMENT = 299,
     ASYMMETRIC = 300,
     AT = 301,
     ATTACH = 302,
     ATTRIBUTE = 303,
     AUTHORIZATION = 304,
     BACKWARD = 305,
     BEFORE = 306,
     BEGIN_P = 307,
     BETWEEN = 308,
     BIGINT = 309,
     BINARY = 310,
     BIT = 311,
     BOOLEAN_P = 312,
     BOTH = 313,
     BY = 314,
     CACHE = 315,
     CALL_P = 316,
     CALLED = 317,
     CASCADE = 318,
     CASCADED = 319,
     CASE = 320,
     CAST = 321,
     CATALOG_P = 322,
     CENTURIES_P = 323,
     CENTURY_P = 324,
     CHAIN = 325,
     CHAR_P = 326,
     CHARACTER = 327,
     CHARACTERISTICS = 328,
     CHECK_P = 329,
     CHECKPOINT = 330,
     CLASS = 331,
     CLOSE = 332,
     CLUSTER = 333,
     COALESCE = 334,
     COLLATE = 335,
     COLLATION = 336,
     COLUMN = 337,
     COLUMNS = 338,
     COMMENT = 339,
     COMMENTS = 340,
     COMMIT = 341,
     COMMITTED = 342,
     COMPRESSION = 343,
     CONCURRENTLY = 344,
     CONFIGURATION = 345,
     CONFLICT = 346,
     CONNECTION = 347,
     CONSTRAINT = 348,
     CONSTRAINTS = 349,
     CONTENT_P = 350,
     CONTINUE_P = 351,
     CONVERSION_P = 352,
     COPY = 353,
     COST = 354,
     CREATE_P = 355,
     CROSS = 356,
     CSV = 357,
     CUBE = 358,
     CURRENT_P = 359,
     CURSOR = 360,
     CYCLE = 361,
     DATA_P = 362,
     DATABASE = 363,
     DAY_P = 364,
     DAYS_P = 365,
     DEALLOCATE = 366,
     DEC = 367,
     DECADE_P = 368,
     DECADES_P = 369,
     DECIDE = 370,
     DECIMAL_P = 371,
     DECLARE = 372,
     DEFAULT = 373,
     DEFAULTS = 374,
     DEFERRABLE = 375,
     DEFERRED = 376,
     DEFINER = 377,
     DELETE_P = 378,
     DELIMITER = 379,
     DELIMITERS = 380,
     DEPENDS = 381,
     DESC_P = 382,
     DESCRIBE = 383,
     DETACH = 384,
     DICTIONARY = 385,
     DISABLE_P = 386,
     DISCARD = 387,
     DISTINCT = 388,
     DO = 389,
     DOCUMENT_P = 390,
     DOMAIN_P = 391,
     DOUBLE_P = 392,
     DROP = 393,
     EACH = 394,
     ELSE = 395,
     ENABLE_P = 396,
     ENCODING = 397,
     ENCRYPTED = 398,
     END_P = 399,
     ENUM_P = 400,
     ESCAPE = 401,
     EVENT = 402,
     EXCEPT = 403,
     EXCLUDE = 404,
     EXCLUDING = 405,
     EXCLUSIVE = 406,
     EXECUTE = 407,
     EXISTS = 408,
     EXPLAIN = 409,
     EXPORT_P = 410,
     EXPORT_STATE = 411,
     EXTENSION = 412,
     EXTENSIONS = 413,
     EXTERNAL = 414,
     EXTRACT = 415,
     FALSE_P = 416,
     FAMILY = 417,
     FETCH = 418,
     FILTER = 419,
     FIRST_P = 420,
     FLOAT_P = 421,
     FOLLOWING = 422,
     FOR = 423,
     FORCE = 424,
     FOREIGN = 425,
     FORWARD = 426,
     FREEZE = 427,
     FROM = 428,
     FULL = 429,
     FUNCTION = 430,
     FUNCTIONS = 431,
     GENERATED = 432,
     GLOB = 433,
     GLOBAL = 434,
     GRANT = 435,
     GRANTED = 436,
     GROUP_P = 437,
     GROUPING = 438,
     GROUPING_ID = 439,
     GROUPS = 440,
     HANDLER = 441,
     HAVING = 442,
     HEADER_P = 443,
     HOLD = 444,
     HOUR_P = 445,
     HOURS_P = 446,
     IDENTITY_P = 447,
     IF_P = 448,
     IGNORE_P = 449,
     ILIKE = 450,
     IMMEDIATE = 451,
     IMMUTABLE = 452,
     IMPLICIT_P = 453,
     IMPORT_P = 454,
     IN_P = 455,
     INCLUDE_P = 456,
     INCLUDING = 457,
     INCREMENT = 458,
     INDEX = 459,
     INDEXES = 460,
     INHERIT = 461,
     INHERITS = 462,
     INITIALLY = 463,
     INLINE_P = 464,
     INNER_P = 465,
     INOUT = 466,
     INPUT_P = 467,
     INSENSITIVE = 468,
     INSERT = 469,
     INSTALL = 470,
     INSTEAD = 471,
     INT_P = 472,
     INTEGER = 473,
     INTERSECT = 474,
     INTERVAL = 475,
     INTO = 476,
     INVOKER = 477,
     IS = 478,
     ISNULL = 479,
     ISOLATION = 480,
     JOIN = 481,
     JSON = 482,
     KEY = 483,
     LABEL = 484,
     LANGUAGE = 485,
     LARGE_P = 486,
     LAST_P = 487,
     LATERAL_P = 488,
     LEADING = 489,
     LEAKPROOF = 490,
     LEFT = 491,
     LEVEL = 492,
     LIKE = 493,
     LIMIT = 494,
     LISTEN = 495,
     LOAD = 496,
     LOCAL = 497,
     LOCATION = 498,
     LOCK_P = 499,
     LOCKED = 500,
     LOGGED = 501,
     MACRO = 502,
     MAP = 503,
     MAPPING = 504,
     MATCH = 505,
     MATERIALIZED = 506,
     MAXIMIZE = 507,
     MAXVALUE = 508,
     METHOD = 509,
     MICROSECOND_P = 510,
     MICROSECONDS_P = 511,
     MILLENNIA_P = 512,
     MILLENNIUM_P = 513,
     MILLISECOND_P = 514,
     MILLISECONDS_P = 515,
     MINIMIZE = 516,
     MINUTE_P = 517,
     MINUTES_P = 518,
     MINVALUE = 519,
     MODE = 520,
     MONTH_P = 521,
     MONTHS_P = 522,
     MOVE = 523,
     NAME_P = 524,
     NAMES = 525,
     NATIONAL = 526,
     NATURAL = 527,
     NCHAR = 528,
     NEW = 529,
     NEXT = 530,
     NO = 531,
     NONE = 532,
     NOT = 533,
     NOTHING = 534,
     NOTIFY = 535,
     NOTNULL = 536,
     NOWAIT = 537,
     NULL_P = 538,
     NULLIF = 539,
     NULLS_P = 540,
     NUMERIC = 541,
     OBJECT_P = 542,
     OF = 543,
     OFF = 544,
     OFFSET = 545,
     OIDS = 546,
     OLD = 547,
     ON = 548,
     ONLY = 549,
     OPERATOR = 550,
     OPTION = 551,
     OPTIONS = 552,
     OR = 553,
     ORDER = 554,
     ORDINALITY = 555,
     OTHERS = 556,
     OUT_P = 557,
     OUTER_P = 558,
     OVER = 559,
     OVERLAPS = 560,
     OVERLAY = 561,
     OVERRIDING = 562,
     OWNED = 563,
     OWNER = 564,
     PARALLEL = 565,
     PARSER = 566,
     PARTIAL = 567,
     PARTITION = 568,
     PASSING = 569,
     PASSWORD = 570,
     PER = 571,
     PERCENT = 572,
     PERSISTENT = 573,
     PIVOT = 574,
     PIVOT_LONGER = 575,
     PIVOT_WIDER = 576,
     PLACING = 577,
     PLANS = 578,
     POLICY = 579,
     POSITION = 580,
     POSITIONAL = 581,
     PRAGMA_P = 582,
     PRECEDING = 583,
     PRECISION = 584,
     PREPARE = 585,
     PREPARED = 586,
     PRESERVE = 587,
     PRIMARY = 588,
     PRIOR = 589,
     PRIVILEGES = 590,
     PROCEDURAL = 591,
     PROCEDURE = 592,
     PROGRAM = 593,
     PUBLICATION = 594,
     QUALIFY = 595,
     QUARTER_P = 596,
     QUARTERS_P = 597,
     QUOTE = 598,
     RANGE = 599,
     READ_P = 600,
     REAL = 601,
     REASSIGN = 602,
     RECHECK = 603,
     RECURSIVE = 604,
     REF = 605,
     REFERENCES = 606,
     REFERENCING = 607,
     REFRESH = 608,
     REINDEX = 609,
     RELATIVE_P = 610,
     RELEASE = 611,
     RENAME = 612,
     REPEATABLE = 613,
     REPLACE = 614,
     REPLICA = 615,
     RESET = 616,
     RESPECT_P = 617,
     RESTART = 618,
     RESTRICT = 619,
     RETURNING = 620,
     RETURNS = 621,
     REVOKE = 622,
     RIGHT = 623,
     ROLE = 624,
     ROLLBACK = 625,
     ROLLUP = 626,
     ROW = 627,
     ROWS = 628,
     RULE = 629,
     SAMPLE = 630,
     SAVEPOINT = 631,
     SCHEMA = 632,
     SCHEMAS = 633,
     SCOPE = 634,
     SCROLL = 635,
     SEARCH = 636,
     SECOND_P = 637,
     SECONDS_P = 638,
     SECRET = 639,
     SECURITY = 640,
     SELECT = 641,
     SEMI = 642,
     SEQUENCE = 643,
     SEQUENCES = 644,
     SERIALIZABLE = 645,
     SERVER = 646,
     SESSION = 647,
     SET = 648,
     SETOF = 649,
     SETS = 650,
     SHARE = 651,
     SHOW = 652,
     SIMILAR = 653,
     SIMPLE = 654,
     SKIP = 655,
     SMALLINT = 656,
     SNAPSHOT = 657,
     SOME = 658,
     SQL_P = 659,
     STABLE = 660,
     STANDALONE_P = 661,
     START = 662,
     STATEMENT = 663,
     STATISTICS = 664,
     STDIN = 665,
     STDOUT = 666,
     STORAGE = 667,
     STORED = 668,
     STRIP_P = 669,
     STRUCT = 670,
     SUBSCRIPTION = 671,
     SUBSTRING = 672,
     SUCH = 673,
     SUMMARIZE = 674,
     SYMMETRIC = 675,
     SYSID = 676,
     SYSTEM_P = 677,
     TABLE = 678,
     TABLES = 679,
     TABLESAMPLE = 680,
     TABLESPACE = 681,
     TEMP = 682,
     TEMPLATE = 683,
     TEMPORARY = 684,
     TEXT_P = 685,
     THAT = 686,
     THEN = 687,
     TIES = 688,
     TIME = 689,
     TIMESTAMP = 690,
     TO = 691,
     TRAILING = 692,
     TRANSACTION = 693,
     TRANSFORM = 694,
     TREAT = 695,
     TRIGGER = 696,
     TRIM = 697,
     TRUE_P = 698,
     TRUNCATE = 699,
     TRUSTED = 700,
     TRY_CAST = 701,
     TYPE_P = 702,
     TYPES_P = 703,
     UNBOUNDED = 704,
     UNCOMMITTED = 705,
     UNENCRYPTED = 706,
     UNION = 707,
     UNIQUE = 708,
     UNKNOWN = 709,
     UNLISTEN = 710,
     UNLOGGED = 711,
     UNPIVOT = 712,
     UNTIL = 713,
     UPDATE = 714,
     USE_P = 715,
     USER = 716,
     USING = 717,
     VACUUM = 718,
     VALID = 719,
     VALIDATE = 720,
     VALIDATOR = 721,
     VALUE_P = 722,
     VALUES = 723,
     VARCHAR = 724,
     VARIABLE_P = 725,
     VARIADIC = 726,
     VARYING = 727,
     VERBOSE = 728,
     VERSION_P = 729,
     VIEW = 730,
     VIEWS = 731,
     VIRTUAL = 732,
     VOLATILE = 733,
     WEEK_P = 734,
     WEEKS_P = 735,
     WHEN = 736,
     WHERE = 737,
     WHITESPACE_P = 738,
     WINDOW = 739,
     WITH = 740,
     WITHIN = 741,
     WITHOUT = 742,
     WORK = 743,
     WRAPPER = 744,
     WRITE_P = 745,
     XML_P = 746,
     XMLATTRIBUTES = 747,
     XMLCONCAT = 748,
     XMLELEMENT = 749,
     XMLEXISTS = 750,
     XMLFOREST = 751,
     XMLNAMESPACES = 752,
     XMLPARSE = 753,
     XMLPI = 754,
     XMLROOT = 755,
     XMLSERIALIZE = 756,
     XMLTABLE = 757,
     YEAR_P = 758,
     YEARS_P = 759,
     YES_P = 760,
     ZONE = 761,
     NOT_LA = 762,
     NULLS_LA = 763,
     WITH_LA = 764,
     WHEN_DECIDE = 765,
     POSTFIXOP = 766,
     UMINUS = 767
   };
#endif
/* Tokens.  */
#define IDENT 258
#define FCONST 259
#define SCONST 260
#define BCONST 261
#define XCONST 262
#define Op 263
#define ICONST 264
#define PARAM 265
#define TYPECAST 266
#define DOT_DOT 267
#define COLON_EQUALS 268
#define EQUALS_GREATER 269
#define INTEGER_DIVISION 270
#define POWER_OF 271
#define LAMBDA_ARROW 272
#define DOUBLE_ARROW 273
#define LESS_EQUALS 274
#define GREATER_EQUALS 275
#define NOT_EQUALS 276
#define ABORT_P 277
#define ABSOLUTE_P 278
#define ACCESS 279
#define ACTION 280
#define ADD_P 281
#define ADMIN 282
#define AFTER 283
#define AGGREGATE 284
#define ALL 285
#define ALSO 286
#define ALTER 287
#define ALWAYS 288
#define ANALYSE 289
#define ANALYZE 290
#define AND 291
#define ANTI 292
#define ANY 293
#define ARRAY 294
#define AS 295
#define ASC_P 296
#define ASOF 297
#define ASSERTION 298
#define ASSIGNMENT 299
#define ASYMMETRIC 300
#define AT 301
#define ATTACH 302
#define ATTRIBUTE 303
#define AUTHORIZATION 304
#define BACKWARD 305
#define BEFORE 306
#define BEGIN_P 307
#define BETWEEN 308
#define BIGINT 309
#define BINARY 310
#define BIT 311
#define BOOLEAN_P 312
#define BOTH 313
#define BY 314
#define CACHE 315
#define CALL_P 316
#define CALLED 317
#define CASCADE 318
#define CASCADED 319
#define CASE 320
#define CAST 321
#define CATALOG_P 322
#define CENTURIES_P 323
#define CENTURY_P 324
#define CHAIN 325
#define CHAR_P 326
#define CHARACTER 327
#define CHARACTERISTICS 328
#define CHECK_P 329
#define CHECKPOINT 330
#define CLASS 331
#define CLOSE 332
#define CLUSTER 333
#define COALESCE 334
#define COLLATE 335
#define COLLATION 336
#define COLUMN 337
#define COLUMNS 338
#define COMMENT 339
#define COMMENTS 340
#define COMMIT 341
#define COMMITTED 342
#define COMPRESSION 343
#define CONCURRENTLY 344
#define CONFIGURATION 345
#define CONFLICT 346
#define CONNECTION 347
#define CONSTRAINT 348
#define CONSTRAINTS 349
#define CONTENT_P 350
#define CONTINUE_P 351
#define CONVERSION_P 352
#define COPY 353
#define COST 354
#define CREATE_P 355
#define CROSS 356
#define CSV 357
#define CUBE 358
#define CURRENT_P 359
#define CURSOR 360
#define CYCLE 361
#define DATA_P 362
#define DATABASE 363
#define DAY_P 364
#define DAYS_P 365
#define DEALLOCATE 366
#define DEC 367
#define DECADE_P 368
#define DECADES_P 369
#define DECIDE 370
#define DECIMAL_P 371
#define DECLARE 372
#define DEFAULT 373
#define DEFAULTS 374
#define DEFERRABLE 375
#define DEFERRED 376
#define DEFINER 377
#define DELETE_P 378
#define DELIMITER 379
#define DELIMITERS 380
#define DEPENDS 381
#define DESC_P 382
#define DESCRIBE 383
#define DETACH 384
#define DICTIONARY 385
#define DISABLE_P 386
#define DISCARD 387
#define DISTINCT 388
#define DO 389
#define DOCUMENT_P 390
#define DOMAIN_P 391
#define DOUBLE_P 392
#define DROP 393
#define EACH 394
#define ELSE 395
#define ENABLE_P 396
#define ENCODING 397
#define ENCRYPTED 398
#define END_P 399
#define ENUM_P 400
#define ESCAPE 401
#define EVENT 402
#define EXCEPT 403
#define EXCLUDE 404
#define EXCLUDING 405
#define EXCLUSIVE 406
#define EXECUTE 407
#define EXISTS 408
#define EXPLAIN 409
#define EXPORT_P 410
#define EXPORT_STATE 411
#define EXTENSION 412
#define EXTENSIONS 413
#define EXTERNAL 414
#define EXTRACT 415
#define FALSE_P 416
#define FAMILY 417
#define FETCH 418
#define FILTER 419
#define FIRST_P 420
#define FLOAT_P 421
#define FOLLOWING 422
#define FOR 423
#define FORCE 424
#define FOREIGN 425
#define FORWARD 426
#define FREEZE 427
#define FROM 428
#define FULL 429
#define FUNCTION 430
#define FUNCTIONS 431
#define GENERATED 432
#define GLOB 433
#define GLOBAL 434
#define GRANT 435
#define GRANTED 436
#define GROUP_P 437
#define GROUPING 438
#define GROUPING_ID 439
#define GROUPS 440
#define HANDLER 441
#define HAVING 442
#define HEADER_P 443
#define HOLD 444
#define HOUR_P 445
#define HOURS_P 446
#define IDENTITY_P 447
#define IF_P 448
#define IGNORE_P 449
#define ILIKE 450
#define IMMEDIATE 451
#define IMMUTABLE 452
#define IMPLICIT_P 453
#define IMPORT_P 454
#define IN_P 455
#define INCLUDE_P 456
#define INCLUDING 457
#define INCREMENT 458
#define INDEX 459
#define INDEXES 460
#define INHERIT 461
#define INHERITS 462
#define INITIALLY 463
#define INLINE_P 464
#define INNER_P 465
#define INOUT 466
#define INPUT_P 467
#define INSENSITIVE 468
#define INSERT 469
#define INSTALL 470
#define INSTEAD 471
#define INT_P 472
#define INTEGER 473
#define INTERSECT 474
#define INTERVAL 475
#define INTO 476
#define INVOKER 477
#define IS 478
#define ISNULL 479
#define ISOLATION 480
#define JOIN 481
#define JSON 482
#define KEY 483
#define LABEL 484
#define LANGUAGE 485
#define LARGE_P 486
#define LAST_P 487
#define LATERAL_P 488
#define LEADING 489
#define LEAKPROOF 490
#define LEFT 491
#define LEVEL 492
#define LIKE 493
#define LIMIT 494
#define LISTEN 495
#define LOAD 496
#define LOCAL 497
#define LOCATION 498
#define LOCK_P 499
#define LOCKED 500
#define LOGGED 501
#define MACRO 502
#define MAP 503
#define MAPPING 504
#define MATCH 505
#define MATERIALIZED 506
#define MAXIMIZE 507
#define MAXVALUE 508
#define METHOD 509
#define MICROSECOND_P 510
#define MICROSECONDS_P 511
#define MILLENNIA_P 512
#define MILLENNIUM_P 513
#define MILLISECOND_P 514
#define MILLISECONDS_P 515
#define MINIMIZE 516
#define MINUTE_P 517
#define MINUTES_P 518
#define MINVALUE 519
#define MODE 520
#define MONTH_P 521
#define MONTHS_P 522
#define MOVE 523
#define NAME_P 524
#define NAMES 525
#define NATIONAL 526
#define NATURAL 527
#define NCHAR 528
#define NEW 529
#define NEXT 530
#define NO 531
#define NONE 532
#define NOT 533
#define NOTHING 534
#define NOTIFY 535
#define NOTNULL 536
#define NOWAIT 537
#define NULL_P 538
#define NULLIF 539
#define NULLS_P 540
#define NUMERIC 541
#define OBJECT_P 542
#define OF 543
#define OFF 544
#define OFFSET 545
#define OIDS 546
#define OLD 547
#define ON 548
#define ONLY 549
#define OPERATOR 550
#define OPTION 551
#define OPTIONS 552
#define OR 553
#define ORDER 554
#define ORDINALITY 555
#define OTHERS 556
#define OUT_P 557
#define OUTER_P 558
#define OVER 559
#define OVERLAPS 560
#define OVERLAY 561
#define OVERRIDING 562
#define OWNED 563
#define OWNER 564
#define PARALLEL 565
#define PARSER 566
#define PARTIAL 567
#define PARTITION 568
#define PASSING 569
#define PASSWORD 570
#define PER 571
#define PERCENT 572
#define PERSISTENT 573
#define PIVOT 574
#define PIVOT_LONGER 575
#define PIVOT_WIDER 576
#define PLACING 577
#define PLANS 578
#define POLICY 579
#define POSITION 580
#define POSITIONAL 581
#define PRAGMA_P 582
#define PRECEDING 583
#define PRECISION 584
#define PREPARE 585
#define PREPARED 586
#define PRESERVE 587
#define PRIMARY 588
#define PRIOR 589
#define PRIVILEGES 590
#define PROCEDURAL 591
#define PROCEDURE 592
#define PROGRAM 593
#define PUBLICATION 594
#define QUALIFY 595
#define QUARTER_P 596
#define QUARTERS_P 597
#define QUOTE 598
#define RANGE 599
#define READ_P 600
#define REAL 601
#define REASSIGN 602
#define RECHECK 603
#define RECURSIVE 604
#define REF 605
#define REFERENCES 606
#define REFERENCING 607
#define REFRESH 608
#define REINDEX 609
#define RELATIVE_P 610
#define RELEASE 611
#define RENAME 612
#define REPEATABLE 613
#define REPLACE 614
#define REPLICA 615
#define RESET 616
#define RESPECT_P 617
#define RESTART 618
#define RESTRICT 619
#define RETURNING 620
#define RETURNS 621
#define REVOKE 622
#define RIGHT 623
#define ROLE 624
#define ROLLBACK 625
#define ROLLUP 626
#define ROW 627
#define ROWS 628
#define RULE 629
#define SAMPLE 630
#define SAVEPOINT 631
#define SCHEMA 632
#define SCHEMAS 633
#define SCOPE 634
#define SCROLL 635
#define SEARCH 636
#define SECOND_P 637
#define SECONDS_P 638
#define SECRET 639
#define SECURITY 640
#define SELECT 641
#define SEMI 642
#define SEQUENCE 643
#define SEQUENCES 644
#define SERIALIZABLE 645
#define SERVER 646
#define SESSION 647
#define SET 648
#define SETOF 649
#define SETS 650
#define SHARE 651
#define SHOW 652
#define SIMILAR 653
#define SIMPLE 654
#define SKIP 655
#define SMALLINT 656
#define SNAPSHOT 657
#define SOME 658
#define SQL_P 659
#define STABLE 660
#define STANDALONE_P 661
#define START 662
#define STATEMENT 663
#define STATISTICS 664
#define STDIN 665
#define STDOUT 666
#define STORAGE 667
#define STORED 668
#define STRIP_P 669
#define STRUCT 670
#define SUBSCRIPTION 671
#define SUBSTRING 672
#define SUCH 673
#define SUMMARIZE 674
#define SYMMETRIC 675
#define SYSID 676
#define SYSTEM_P 677
#define TABLE 678
#define TABLES 679
#define TABLESAMPLE 680
#define TABLESPACE 681
#define TEMP 682
#define TEMPLATE 683
#define TEMPORARY 684
#define TEXT_P 685
#define THAT 686
#define THEN 687
#define TIES 688
#define TIME 689
#define TIMESTAMP 690
#define TO 691
#define TRAILING 692
#define TRANSACTION 693
#define TRANSFORM 694
#define TREAT 695
#define TRIGGER 696
#define TRIM 697
#define TRUE_P 698
#define TRUNCATE 699
#define TRUSTED 700
#define TRY_CAST 701
#define TYPE_P 702
#define TYPES_P 703
#define UNBOUNDED 704
#define UNCOMMITTED 705
#define UNENCRYPTED 706
#define UNION 707
#define UNIQUE 708
#define UNKNOWN 709
#define UNLISTEN 710
#define UNLOGGED 711
#define UNPIVOT 712
#define UNTIL 713
#define UPDATE 714
#define USE_P 715
#define USER 716
#define USING 717
#define VACUUM 718
#define VALID 719
#define VALIDATE 720
#define VALIDATOR 721
#define VALUE_P 722
#define VALUES 723
#define VARCHAR 724
#define VARIABLE_P 725
#define VARIADIC 726
#define VARYING 727
#define VERBOSE 728
#define VERSION_P 729
#define VIEW 730
#define VIEWS 731
#define VIRTUAL 732
#define VOLATILE 733
#define WEEK_P 734
#define WEEKS_P 735
#define WHEN 736
#define WHERE 737
#define WHITESPACE_P 738
#define WINDOW 739
#define WITH 740
#define WITHIN 741
#define WITHOUT 742
#define WORK 743
#define WRAPPER 744
#define WRITE_P 745
#define XML_P 746
#define XMLATTRIBUTES 747
#define XMLCONCAT 748
#define XMLELEMENT 749
#define XMLEXISTS 750
#define XMLFOREST 751
#define XMLNAMESPACES 752
#define XMLPARSE 753
#define XMLPI 754
#define XMLROOT 755
#define XMLSERIALIZE 756
#define XMLTABLE 757
#define YEAR_P 758
#define YEARS_P 759
#define YES_P 760
#define ZONE 761
#define NOT_LA 762
#define NULLS_LA 763
#define WITH_LA 764
#define WHEN_DECIDE 765
#define POSTFIXOP 766
#define UMINUS 767




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 18 "third_party/libpg_query/grammar/grammar.y"
{
	core_YYSTYPE		core_yystype;
	/* these fields must match core_YYSTYPE: */
	int					ival;
	char				*str;
	const char			*keyword;
	const char          *conststr;

	char				chr;
	bool				boolean;
	PGJoinType			jtype;
	PGDropBehavior		dbehavior;
	PGOnCommitAction		oncommit;
	PGOnCreateConflict		oncreateconflict;
	PGList				*list;
	PGNode				*node;
	PGValue				*value;
	PGObjectType			objtype;
	PGTypeName			*typnam;
	PGObjectWithArgs		*objwithargs;
	PGDefElem				*defelt;
	PGSortBy				*sortby;
	PGWindowDef			*windef;
	PGJoinExpr			*jexpr;
	PGIndexElem			*ielem;
	PGAlias				*alias;
	PGRangeVar			*range;
	PGIntoClause			*into;
	PGCTEMaterialize			ctematerialize;
	PGWithClause			*with;
	PGInferClause			*infer;
	PGOnConflictClause	*onconflict;
	PGOnConflictActionAlias onconflictshorthand;
	PGAIndices			*aind;
	PGResTarget			*target;
	PGInsertStmt			*istmt;
	PGVariableSetStmt		*vsetstmt;
	PGOverridingKind       override;
	PGSortByDir            sortorder;
	PGSortByNulls          nullorder;
	PGIgnoreNulls          ignorenulls;
	PGConstrType           constr;
	PGLockClauseStrength lockstrength;
	PGLockWaitPolicy lockwaitpolicy;
	PGSubLinkType subquerytype;
	PGViewCheckOption viewcheckoption;
	PGInsertColumnOrder bynameorposition;
	PGLoadInstallType loadinstalltype;
	PGTransactionStmtType transactiontype;
}
/* Line 1529 of yacc.c.  */
#line 1124 "third_party/libpg_query/grammar/grammar_out.hpp"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif



#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
} YYLTYPE;
# define yyltype YYLTYPE /* obsolescent; will be withdrawn */
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif


