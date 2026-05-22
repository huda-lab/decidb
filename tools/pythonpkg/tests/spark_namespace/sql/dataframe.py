from .. import USE_ACTUAL_SPARK

if USE_ACTUAL_SPARK:
    from pyspark.sql.dataframe import *
else:
    from decidb.experimental.spark.sql.dataframe import *
