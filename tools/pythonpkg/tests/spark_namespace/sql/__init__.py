from .. import USE_ACTUAL_SPARK

if USE_ACTUAL_SPARK:
    from pyspark.sql import SparkSession
else:
    from decidb.experimental.spark.sql import SparkSession
