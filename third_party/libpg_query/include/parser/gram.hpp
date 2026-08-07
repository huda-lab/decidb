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
     BOOL_P = 312,
     BOOLEAN_P = 313,
     BOTH = 314,
     BY = 315,
     CACHE = 316,
     CALL_P = 317,
     CALLED = 318,
     CASCADE = 319,
     CASCADED = 320,
     CASE = 321,
     CAST = 322,
     CATALOG_P = 323,
     CENTURIES_P = 324,
     CENTURY_P = 325,
     CHAIN = 326,
     CHAR_P = 327,
     CHARACTER = 328,
     CHARACTERISTICS = 329,
     CHECK_P = 330,
     CHECKPOINT = 331,
     CLASS = 332,
     CLOSE = 333,
     CLUSTER = 334,
     COALESCE = 335,
     COLLATE = 336,
     COLLATION = 337,
     COLUMN = 338,
     COLUMNS = 339,
     COMMENT = 340,
     COMMENTS = 341,
     COMMIT = 342,
     COMMITTED = 343,
     COMPRESSION = 344,
     CONCURRENTLY = 345,
     CONFIGURATION = 346,
     CONFLICT = 347,
     CONNECTION = 348,
     CONSTRAINT = 349,
     CONSTRAINTS = 350,
     CONTENT_P = 351,
     CONTINUE_P = 352,
     CONVERSION_P = 353,
     COPY = 354,
     COST = 355,
     CREATE_P = 356,
     CROSS = 357,
     CSV = 358,
     CUBE = 359,
     CURRENT_P = 360,
     CURSOR = 361,
     CYCLE = 362,
     DATA_P = 363,
     DATABASE = 364,
     DAY_P = 365,
     DAYS_P = 366,
     DEALLOCATE = 367,
     DEC = 368,
     DECADE_P = 369,
     DECADES_P = 370,
     DECIDE = 371,
     DECIMAL_P = 372,
     DECLARE = 373,
     DEFAULT = 374,
     DEFAULTS = 375,
     DEFERRABLE = 376,
     DEFERRED = 377,
     DEFINER = 378,
     DELETE_P = 379,
     DELIMITER = 380,
     DELIMITERS = 381,
     DEPENDS = 382,
     DESC_P = 383,
     DESCRIBE = 384,
     DETACH = 385,
     DICTIONARY = 386,
     DISABLE_P = 387,
     DISCARD = 388,
     DISTINCT = 389,
     DO = 390,
     DOCUMENT_P = 391,
     DOMAIN_P = 392,
     DOUBLE_P = 393,
     DROP = 394,
     EACH = 395,
     ELSE = 396,
     ENABLE_P = 397,
     ENCODING = 398,
     ENCRYPTED = 399,
     END_P = 400,
     ENUM_P = 401,
     ESCAPE = 402,
     EVENT = 403,
     EXCEPT = 404,
     EXCLUDE = 405,
     EXCLUDING = 406,
     EXCLUSIVE = 407,
     EXECUTE = 408,
     EXISTS = 409,
     EXPLAIN = 410,
     EXPORT_P = 411,
     EXPORT_STATE = 412,
     EXTENSION = 413,
     EXTENSIONS = 414,
     EXTERNAL = 415,
     EXTRACT = 416,
     FALSE_P = 417,
     FAMILY = 418,
     FETCH = 419,
     FILTER = 420,
     FIRST_P = 421,
     FLOAT_P = 422,
     FOLLOWING = 423,
     FOR = 424,
     FORCE = 425,
     FOREIGN = 426,
     FORWARD = 427,
     FREEZE = 428,
     FROM = 429,
     FULL = 430,
     FUNCTION = 431,
     FUNCTIONS = 432,
     GENERATED = 433,
     GLOB = 434,
     GLOBAL = 435,
     GRANT = 436,
     GRANTED = 437,
     GROUP_P = 438,
     GROUPING = 439,
     GROUPING_ID = 440,
     GROUPS = 441,
     HANDLER = 442,
     HAVING = 443,
     HEADER_P = 444,
     HOLD = 445,
     HOUR_P = 446,
     HOURS_P = 447,
     IDENTITY_P = 448,
     IF_P = 449,
     IGNORE_P = 450,
     ILIKE = 451,
     IMMEDIATE = 452,
     IMMUTABLE = 453,
     IMPLICIT_P = 454,
     IMPORT_P = 455,
     IN_P = 456,
     INCLUDE_P = 457,
     INCLUDING = 458,
     INCREMENT = 459,
     INDEX = 460,
     INDEXES = 461,
     INHERIT = 462,
     INHERITS = 463,
     INITIALLY = 464,
     INLINE_P = 465,
     INNER_P = 466,
     INOUT = 467,
     INPUT_P = 468,
     INSENSITIVE = 469,
     INSERT = 470,
     INSTALL = 471,
     INSTEAD = 472,
     INT_P = 473,
     INTEGER = 474,
     INTERSECT = 475,
     INTERVAL = 476,
     INTO = 477,
     INVOKER = 478,
     IS = 479,
     ISNULL = 480,
     ISOLATION = 481,
     JOIN = 482,
     JSON = 483,
     KEY = 484,
     LABEL = 485,
     LANGUAGE = 486,
     LARGE_P = 487,
     LAST_P = 488,
     LATERAL_P = 489,
     LEADING = 490,
     LEAKPROOF = 491,
     LEFT = 492,
     LEVEL = 493,
     LIKE = 494,
     LIMIT = 495,
     LISTEN = 496,
     LOAD = 497,
     LOCAL = 498,
     LOCATION = 499,
     LOCK_P = 500,
     LOCKED = 501,
     LOGGED = 502,
     MACRO = 503,
     MAP = 504,
     MAPPING = 505,
     MATCH = 506,
     MATERIALIZED = 507,
     MAXIMIZE = 508,
     MAXVALUE = 509,
     METHOD = 510,
     MICROSECOND_P = 511,
     MICROSECONDS_P = 512,
     MILLENNIA_P = 513,
     MILLENNIUM_P = 514,
     MILLISECOND_P = 515,
     MILLISECONDS_P = 516,
     MINIMIZE = 517,
     MINUTE_P = 518,
     MINUTES_P = 519,
     MINVALUE = 520,
     MODE = 521,
     MONTH_P = 522,
     MONTHS_P = 523,
     MOVE = 524,
     NAME_P = 525,
     NAMES = 526,
     NATIONAL = 527,
     NATURAL = 528,
     NCHAR = 529,
     NEW = 530,
     NEXT = 531,
     NO = 532,
     NONE = 533,
     NOT = 534,
     NOTHING = 535,
     NOTIFY = 536,
     NOTNULL = 537,
     NOWAIT = 538,
     NULL_P = 539,
     NULLIF = 540,
     NULLS_P = 541,
     NUMERIC = 542,
     OBJECT_P = 543,
     OF = 544,
     OFF = 545,
     OFFSET = 546,
     OIDS = 547,
     OLD = 548,
     ON = 549,
     ONLY = 550,
     OPERATOR = 551,
     OPTION = 552,
     OPTIONS = 553,
     OR = 554,
     ORDER = 555,
     ORDINALITY = 556,
     OTHERS = 557,
     OUT_P = 558,
     OUTER_P = 559,
     OVER = 560,
     OVERLAPS = 561,
     OVERLAY = 562,
     OVERRIDING = 563,
     OWNED = 564,
     OWNER = 565,
     PARALLEL = 566,
     PARSER = 567,
     PARTIAL = 568,
     PARTITION = 569,
     PASSING = 570,
     PASSWORD = 571,
     PER = 572,
     PERCENT = 573,
     PERSISTENT = 574,
     PIVOT = 575,
     PIVOT_LONGER = 576,
     PIVOT_WIDER = 577,
     PLACING = 578,
     PLANS = 579,
     POLICY = 580,
     POSITION = 581,
     POSITIONAL = 582,
     PRAGMA_P = 583,
     PRECEDING = 584,
     PRECISION = 585,
     PREPARE = 586,
     PREPARED = 587,
     PRESERVE = 588,
     PRIMARY = 589,
     PRIOR = 590,
     PRIVILEGES = 591,
     PROCEDURAL = 592,
     PROCEDURE = 593,
     PROGRAM = 594,
     PUBLICATION = 595,
     QUALIFY = 596,
     QUARTER_P = 597,
     QUARTERS_P = 598,
     QUOTE = 599,
     RANGE = 600,
     READ_P = 601,
     REAL = 602,
     REASSIGN = 603,
     RECHECK = 604,
     RECURSIVE = 605,
     REF = 606,
     REFERENCES = 607,
     REFERENCING = 608,
     REFRESH = 609,
     REINDEX = 610,
     RELATIVE_P = 611,
     RELEASE = 612,
     RENAME = 613,
     REPEATABLE = 614,
     REPLACE = 615,
     REPLICA = 616,
     RESET = 617,
     RESPECT_P = 618,
     RESTART = 619,
     RESTRICT = 620,
     RETURNING = 621,
     RETURNS = 622,
     REVOKE = 623,
     RIGHT = 624,
     ROLE = 625,
     ROLLBACK = 626,
     ROLLUP = 627,
     ROW = 628,
     ROWS = 629,
     RULE = 630,
     SAMPLE = 631,
     SAVEPOINT = 632,
     SCHEMA = 633,
     SCHEMAS = 634,
     SCOPE = 635,
     SCROLL = 636,
     SEARCH = 637,
     SECOND_P = 638,
     SECONDS_P = 639,
     SECRET = 640,
     SECURITY = 641,
     SELECT = 642,
     SEMI = 643,
     SEQUENCE = 644,
     SEQUENCES = 645,
     SERIALIZABLE = 646,
     SERVER = 647,
     SESSION = 648,
     SET = 649,
     SETOF = 650,
     SETS = 651,
     SHARE = 652,
     SHOW = 653,
     SIMILAR = 654,
     SIMPLE = 655,
     SKIP = 656,
     SMALLINT = 657,
     SNAPSHOT = 658,
     SOME = 659,
     SQL_P = 660,
     STABLE = 661,
     STANDALONE_P = 662,
     START = 663,
     STATEMENT = 664,
     STATISTICS = 665,
     STDIN = 666,
     STDOUT = 667,
     STORAGE = 668,
     STORED = 669,
     STRIP_P = 670,
     STRUCT = 671,
     SUBSCRIPTION = 672,
     SUBSTRING = 673,
     SUCH = 674,
     SUMMARIZE = 675,
     SYMMETRIC = 676,
     SYSID = 677,
     SYSTEM_P = 678,
     TABLE = 679,
     TABLES = 680,
     TABLESAMPLE = 681,
     TABLESPACE = 682,
     TEMP = 683,
     TEMPLATE = 684,
     TEMPORARY = 685,
     TEXT_P = 686,
     THAT = 687,
     THEN = 688,
     TIES = 689,
     TIME = 690,
     TIMESTAMP = 691,
     TO = 692,
     TRAILING = 693,
     TRANSACTION = 694,
     TRANSFORM = 695,
     TREAT = 696,
     TRIGGER = 697,
     TRIM = 698,
     TRUE_P = 699,
     TRUNCATE = 700,
     TRUSTED = 701,
     TRY_CAST = 702,
     TYPE_P = 703,
     TYPES_P = 704,
     UNBOUNDED = 705,
     UNCOMMITTED = 706,
     UNENCRYPTED = 707,
     UNION = 708,
     UNIQUE = 709,
     UNKNOWN = 710,
     UNLISTEN = 711,
     UNLOGGED = 712,
     UNPIVOT = 713,
     UNTIL = 714,
     UPDATE = 715,
     USE_P = 716,
     USER = 717,
     USING = 718,
     VACUUM = 719,
     VALID = 720,
     VALIDATE = 721,
     VALIDATOR = 722,
     VALUE_P = 723,
     VALUES = 724,
     VARCHAR = 725,
     VARIABLE_P = 726,
     VARIADIC = 727,
     VARYING = 728,
     VERBOSE = 729,
     VERSION_P = 730,
     VIEW = 731,
     VIEWS = 732,
     VIRTUAL = 733,
     VOLATILE = 734,
     WEEK_P = 735,
     WEEKS_P = 736,
     WHEN = 737,
     WHERE = 738,
     WHITESPACE_P = 739,
     WINDOW = 740,
     WITH = 741,
     WITHIN = 742,
     WITHOUT = 743,
     WORK = 744,
     WRAPPER = 745,
     WRITE_P = 746,
     XML_P = 747,
     XMLATTRIBUTES = 748,
     XMLCONCAT = 749,
     XMLELEMENT = 750,
     XMLEXISTS = 751,
     XMLFOREST = 752,
     XMLNAMESPACES = 753,
     XMLPARSE = 754,
     XMLPI = 755,
     XMLROOT = 756,
     XMLSERIALIZE = 757,
     XMLTABLE = 758,
     YEAR_P = 759,
     YEARS_P = 760,
     YES_P = 761,
     ZONE = 762,
     NOT_LA = 763,
     NULLS_LA = 764,
     WITH_LA = 765,
     WHEN_DECIDE = 766,
     POSTFIXOP = 767,
     UMINUS = 768
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
#define BOOL_P 312
#define BOOLEAN_P 313
#define BOTH 314
#define BY 315
#define CACHE 316
#define CALL_P 317
#define CALLED 318
#define CASCADE 319
#define CASCADED 320
#define CASE 321
#define CAST 322
#define CATALOG_P 323
#define CENTURIES_P 324
#define CENTURY_P 325
#define CHAIN 326
#define CHAR_P 327
#define CHARACTER 328
#define CHARACTERISTICS 329
#define CHECK_P 330
#define CHECKPOINT 331
#define CLASS 332
#define CLOSE 333
#define CLUSTER 334
#define COALESCE 335
#define COLLATE 336
#define COLLATION 337
#define COLUMN 338
#define COLUMNS 339
#define COMMENT 340
#define COMMENTS 341
#define COMMIT 342
#define COMMITTED 343
#define COMPRESSION 344
#define CONCURRENTLY 345
#define CONFIGURATION 346
#define CONFLICT 347
#define CONNECTION 348
#define CONSTRAINT 349
#define CONSTRAINTS 350
#define CONTENT_P 351
#define CONTINUE_P 352
#define CONVERSION_P 353
#define COPY 354
#define COST 355
#define CREATE_P 356
#define CROSS 357
#define CSV 358
#define CUBE 359
#define CURRENT_P 360
#define CURSOR 361
#define CYCLE 362
#define DATA_P 363
#define DATABASE 364
#define DAY_P 365
#define DAYS_P 366
#define DEALLOCATE 367
#define DEC 368
#define DECADE_P 369
#define DECADES_P 370
#define DECIDE 371
#define DECIMAL_P 372
#define DECLARE 373
#define DEFAULT 374
#define DEFAULTS 375
#define DEFERRABLE 376
#define DEFERRED 377
#define DEFINER 378
#define DELETE_P 379
#define DELIMITER 380
#define DELIMITERS 381
#define DEPENDS 382
#define DESC_P 383
#define DESCRIBE 384
#define DETACH 385
#define DICTIONARY 386
#define DISABLE_P 387
#define DISCARD 388
#define DISTINCT 389
#define DO 390
#define DOCUMENT_P 391
#define DOMAIN_P 392
#define DOUBLE_P 393
#define DROP 394
#define EACH 395
#define ELSE 396
#define ENABLE_P 397
#define ENCODING 398
#define ENCRYPTED 399
#define END_P 400
#define ENUM_P 401
#define ESCAPE 402
#define EVENT 403
#define EXCEPT 404
#define EXCLUDE 405
#define EXCLUDING 406
#define EXCLUSIVE 407
#define EXECUTE 408
#define EXISTS 409
#define EXPLAIN 410
#define EXPORT_P 411
#define EXPORT_STATE 412
#define EXTENSION 413
#define EXTENSIONS 414
#define EXTERNAL 415
#define EXTRACT 416
#define FALSE_P 417
#define FAMILY 418
#define FETCH 419
#define FILTER 420
#define FIRST_P 421
#define FLOAT_P 422
#define FOLLOWING 423
#define FOR 424
#define FORCE 425
#define FOREIGN 426
#define FORWARD 427
#define FREEZE 428
#define FROM 429
#define FULL 430
#define FUNCTION 431
#define FUNCTIONS 432
#define GENERATED 433
#define GLOB 434
#define GLOBAL 435
#define GRANT 436
#define GRANTED 437
#define GROUP_P 438
#define GROUPING 439
#define GROUPING_ID 440
#define GROUPS 441
#define HANDLER 442
#define HAVING 443
#define HEADER_P 444
#define HOLD 445
#define HOUR_P 446
#define HOURS_P 447
#define IDENTITY_P 448
#define IF_P 449
#define IGNORE_P 450
#define ILIKE 451
#define IMMEDIATE 452
#define IMMUTABLE 453
#define IMPLICIT_P 454
#define IMPORT_P 455
#define IN_P 456
#define INCLUDE_P 457
#define INCLUDING 458
#define INCREMENT 459
#define INDEX 460
#define INDEXES 461
#define INHERIT 462
#define INHERITS 463
#define INITIALLY 464
#define INLINE_P 465
#define INNER_P 466
#define INOUT 467
#define INPUT_P 468
#define INSENSITIVE 469
#define INSERT 470
#define INSTALL 471
#define INSTEAD 472
#define INT_P 473
#define INTEGER 474
#define INTERSECT 475
#define INTERVAL 476
#define INTO 477
#define INVOKER 478
#define IS 479
#define ISNULL 480
#define ISOLATION 481
#define JOIN 482
#define JSON 483
#define KEY 484
#define LABEL 485
#define LANGUAGE 486
#define LARGE_P 487
#define LAST_P 488
#define LATERAL_P 489
#define LEADING 490
#define LEAKPROOF 491
#define LEFT 492
#define LEVEL 493
#define LIKE 494
#define LIMIT 495
#define LISTEN 496
#define LOAD 497
#define LOCAL 498
#define LOCATION 499
#define LOCK_P 500
#define LOCKED 501
#define LOGGED 502
#define MACRO 503
#define MAP 504
#define MAPPING 505
#define MATCH 506
#define MATERIALIZED 507
#define MAXIMIZE 508
#define MAXVALUE 509
#define METHOD 510
#define MICROSECOND_P 511
#define MICROSECONDS_P 512
#define MILLENNIA_P 513
#define MILLENNIUM_P 514
#define MILLISECOND_P 515
#define MILLISECONDS_P 516
#define MINIMIZE 517
#define MINUTE_P 518
#define MINUTES_P 519
#define MINVALUE 520
#define MODE 521
#define MONTH_P 522
#define MONTHS_P 523
#define MOVE 524
#define NAME_P 525
#define NAMES 526
#define NATIONAL 527
#define NATURAL 528
#define NCHAR 529
#define NEW 530
#define NEXT 531
#define NO 532
#define NONE 533
#define NOT 534
#define NOTHING 535
#define NOTIFY 536
#define NOTNULL 537
#define NOWAIT 538
#define NULL_P 539
#define NULLIF 540
#define NULLS_P 541
#define NUMERIC 542
#define OBJECT_P 543
#define OF 544
#define OFF 545
#define OFFSET 546
#define OIDS 547
#define OLD 548
#define ON 549
#define ONLY 550
#define OPERATOR 551
#define OPTION 552
#define OPTIONS 553
#define OR 554
#define ORDER 555
#define ORDINALITY 556
#define OTHERS 557
#define OUT_P 558
#define OUTER_P 559
#define OVER 560
#define OVERLAPS 561
#define OVERLAY 562
#define OVERRIDING 563
#define OWNED 564
#define OWNER 565
#define PARALLEL 566
#define PARSER 567
#define PARTIAL 568
#define PARTITION 569
#define PASSING 570
#define PASSWORD 571
#define PER 572
#define PERCENT 573
#define PERSISTENT 574
#define PIVOT 575
#define PIVOT_LONGER 576
#define PIVOT_WIDER 577
#define PLACING 578
#define PLANS 579
#define POLICY 580
#define POSITION 581
#define POSITIONAL 582
#define PRAGMA_P 583
#define PRECEDING 584
#define PRECISION 585
#define PREPARE 586
#define PREPARED 587
#define PRESERVE 588
#define PRIMARY 589
#define PRIOR 590
#define PRIVILEGES 591
#define PROCEDURAL 592
#define PROCEDURE 593
#define PROGRAM 594
#define PUBLICATION 595
#define QUALIFY 596
#define QUARTER_P 597
#define QUARTERS_P 598
#define QUOTE 599
#define RANGE 600
#define READ_P 601
#define REAL 602
#define REASSIGN 603
#define RECHECK 604
#define RECURSIVE 605
#define REF 606
#define REFERENCES 607
#define REFERENCING 608
#define REFRESH 609
#define REINDEX 610
#define RELATIVE_P 611
#define RELEASE 612
#define RENAME 613
#define REPEATABLE 614
#define REPLACE 615
#define REPLICA 616
#define RESET 617
#define RESPECT_P 618
#define RESTART 619
#define RESTRICT 620
#define RETURNING 621
#define RETURNS 622
#define REVOKE 623
#define RIGHT 624
#define ROLE 625
#define ROLLBACK 626
#define ROLLUP 627
#define ROW 628
#define ROWS 629
#define RULE 630
#define SAMPLE 631
#define SAVEPOINT 632
#define SCHEMA 633
#define SCHEMAS 634
#define SCOPE 635
#define SCROLL 636
#define SEARCH 637
#define SECOND_P 638
#define SECONDS_P 639
#define SECRET 640
#define SECURITY 641
#define SELECT 642
#define SEMI 643
#define SEQUENCE 644
#define SEQUENCES 645
#define SERIALIZABLE 646
#define SERVER 647
#define SESSION 648
#define SET 649
#define SETOF 650
#define SETS 651
#define SHARE 652
#define SHOW 653
#define SIMILAR 654
#define SIMPLE 655
#define SKIP 656
#define SMALLINT 657
#define SNAPSHOT 658
#define SOME 659
#define SQL_P 660
#define STABLE 661
#define STANDALONE_P 662
#define START 663
#define STATEMENT 664
#define STATISTICS 665
#define STDIN 666
#define STDOUT 667
#define STORAGE 668
#define STORED 669
#define STRIP_P 670
#define STRUCT 671
#define SUBSCRIPTION 672
#define SUBSTRING 673
#define SUCH 674
#define SUMMARIZE 675
#define SYMMETRIC 676
#define SYSID 677
#define SYSTEM_P 678
#define TABLE 679
#define TABLES 680
#define TABLESAMPLE 681
#define TABLESPACE 682
#define TEMP 683
#define TEMPLATE 684
#define TEMPORARY 685
#define TEXT_P 686
#define THAT 687
#define THEN 688
#define TIES 689
#define TIME 690
#define TIMESTAMP 691
#define TO 692
#define TRAILING 693
#define TRANSACTION 694
#define TRANSFORM 695
#define TREAT 696
#define TRIGGER 697
#define TRIM 698
#define TRUE_P 699
#define TRUNCATE 700
#define TRUSTED 701
#define TRY_CAST 702
#define TYPE_P 703
#define TYPES_P 704
#define UNBOUNDED 705
#define UNCOMMITTED 706
#define UNENCRYPTED 707
#define UNION 708
#define UNIQUE 709
#define UNKNOWN 710
#define UNLISTEN 711
#define UNLOGGED 712
#define UNPIVOT 713
#define UNTIL 714
#define UPDATE 715
#define USE_P 716
#define USER 717
#define USING 718
#define VACUUM 719
#define VALID 720
#define VALIDATE 721
#define VALIDATOR 722
#define VALUE_P 723
#define VALUES 724
#define VARCHAR 725
#define VARIABLE_P 726
#define VARIADIC 727
#define VARYING 728
#define VERBOSE 729
#define VERSION_P 730
#define VIEW 731
#define VIEWS 732
#define VIRTUAL 733
#define VOLATILE 734
#define WEEK_P 735
#define WEEKS_P 736
#define WHEN 737
#define WHERE 738
#define WHITESPACE_P 739
#define WINDOW 740
#define WITH 741
#define WITHIN 742
#define WITHOUT 743
#define WORK 744
#define WRAPPER 745
#define WRITE_P 746
#define XML_P 747
#define XMLATTRIBUTES 748
#define XMLCONCAT 749
#define XMLELEMENT 750
#define XMLEXISTS 751
#define XMLFOREST 752
#define XMLNAMESPACES 753
#define XMLPARSE 754
#define XMLPI 755
#define XMLROOT 756
#define XMLSERIALIZE 757
#define XMLTABLE 758
#define YEAR_P 759
#define YEARS_P 760
#define YES_P 761
#define ZONE 762
#define NOT_LA 763
#define NULLS_LA 764
#define WITH_LA 765
#define WHEN_DECIDE 766
#define POSTFIXOP 767
#define UMINUS 768




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 32 "third_party/libpg_query/grammar/grammar.y"
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
#line 1126 "third_party/libpg_query/grammar/grammar_out.hpp"
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


