A python code for matching points on the sphere using healpix.

Examples
--------

```python

# For quick matches, use the match() function

import smatch

nside=512  # healpix nside
maxmatch=1 # return closest match
matches = smatch.match(ra1, dec2, radius, ra2, dec2,
                       nside=nside, maxmatch=maxmatch)

# in the above call, radius can be a scalar or the
# same size as ra1,dec1

# the output matches structure holds the indices of the matches from each data
# set, and the cosine of the distance between them

print(m.dtype.descr)
    [('i1', '<i8'), ('i2', '<i8'), ('cosdist', '<f8')]

# access via the indices. These should match up one to one
ra1matched  = ra1[ matches['i1'] ]
dec1matched = dec1[ matches['i1'] ]
ra2matched  = ra2[ matches['i2'] ]
dec2matched = dec2[ matches['i2'] ]

# set maxmatch=0 to get all matches, set to a positive
# integer to return that many closest matches

all_matches = smatch.match(ra1, dec2, radius, ra2, dec2,
                           nside=nside, maxmatch=0)
some_matches = smatch.match(ra1, dec2, radius, ra2, dec2,
                            nside=nside, maxmatch=3)


# A more flexibile interface is a Catalog.  For example it can
# be used to match the same data set to multiple other data sets

cat=smatch.Catalog(ra1, dec1, radius, nside=nside)
print(cat)
smatch catalog
    nside:               512
    pixel area (sq deg): 0.013114
    npoints:             100

cat.match(ra2, dec2, maxmatch=maxmatch)
cat.match(ra3, dec3, maxmatch=maxmatch)

print("found:",cat.nmatches,"matches")
matches = cat.matches

#
# Writing matches to  file
# 
# This useful if the number of matches is large, and cannot be
# held in memory

# using the convenience function
fname="matches.dat"
smatch.match2file(ra1, dec2, radius, ra2, dec2,
                  file=fname)

# using a the catalog
cat.match2file(fname, ra3, dec3, maxmatch=maxmatch)


# you can read them later
matches=smatch.read_matches(fname)

# if the matches are too large to read, you can use packages
# such as these to read subsets
# recfile: https://github.com/esheldon/recfile
# esutil.recfile: https://github.com/esheldon/esutil
# (the esutil version is a copy of recfile)

from esutil.recfile import Recfile
dtype=smatch.smatch_dtype

start=10000
end=20000
with Recfile(filename, "r", dtype=dtype, delim=' ') as robj:
    data=robj[start:end]

```
