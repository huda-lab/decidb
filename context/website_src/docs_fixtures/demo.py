"""Databases for the live demo (demo.html): Maya's festival, sixteen acts in four genres.

The page loads the `demo-festival` setup into the DeciDB that runs in the browser; the build runs every query of the
page on the native binary against the same setup, so the story's answers are checked like every other page's.
`demo-festival-big` adds the Big table the speed step makes in the browser, at a small size.
"""

FESTIVAL = """CREATE TABLE Acts (act VARCHAR, genre VARCHAR, fee INTEGER, crowd INTEGER, rating DOUBLE);
INSERT INTO Acts VALUES
    ('The Long Hours', 'Rock', 40, 12000, 7.0), ('Copper Lungs', 'Rock', 22, 7000, 8.5),
    ('Night Ferry', 'Rock', 12, 4000, 6.0), ('Static Bloom', 'Rock', 30, 9000, 9.0),
    ('Blue Hour Trio', 'Jazz', 15, 4000, 9.2), ('Late Set Quartet', 'Jazz', 25, 6000, 8.7),
    ('Low Tide Brass', 'Jazz', 8, 2500, 7.5), ('Sunday Standards', 'Jazz', 10, 3000, 6.8),
    ('Pale Signal', 'Electronic', 32, 11000, 7.9), ('Kilowatt Garden', 'Electronic', 18, 7000, 7.2),
    ('Glass Harbor', 'Electronic', 26, 9500, 8.9), ('Tape Loop', 'Electronic', 9, 3500, 6.5),
    ('Hollow Pines', 'Folk', 14, 4500, 8.3), ('Cedar and Smoke', 'Folk', 20, 6000, 9.1),
    ('Field Recordings', 'Folk', 7, 2000, 7.8), ('The Wren Sisters', 'Folk', 16, 5200, 8.0);"""

# the speed step's table: N acts with ratings spread between 5 and 10 (the page sets N; the build checks 2,000)
BIG = """CREATE OR REPLACE TABLE Big AS
SELECT 'act ' || i AS act, ['Rock', 'Jazz', 'Electronic', 'Folk'][1 + i % 4] AS genre,
       round(5 + (hash(i) % 500) / 100.0, 2) AS rating
FROM range({N}) t(i);"""

FIXTURES = {
    "demo-festival": {"setup": FESTIVAL, "big": BIG},
    "demo-festival-big": {"setup": FESTIVAL + "\n" + BIG.replace("{N}", "2000")},
}
