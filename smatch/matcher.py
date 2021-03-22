import functools
import operator

from scipy.spatial import cKDTree
import healpy as hp
import numpy as np
import esutil.coords


class Matcher(object):
    """A class to match catalogs with cKDTree.

    A collaboration of Alex Drlica-Wagner, Matthew Becker, Eli Rykoff.

    Parameters
    ----------
    lon : array-like
        The longitude in degrees.
    lat : array-like
        The latitude in degrees.
    """
    def __init__(self, lon, lat):
        self.lon = lon
        self.lat = lat
        coords = hp.rotator.dir2vec(lon, lat, lonlat=True).T
        self.tree = cKDTree(coords, compact_nodes=False, balanced_tree=False)

    def query_knn(self, lon, lat, k=1, distance_upper_bound=None, return_indices=False):
        """Find the `k` nearest neighbors of each point in (lon, lat) in
        the points held by the matcher.

        Parameters
        ----------
        lon : array-like, float
            The longitude in degrees.
        lat : array-like, float
            The latitude in degrees.
        k : `int`, optional
            The number of nearest neighbors to find.
        distance_upper_bound : `float`, optional
            The maximum allowed distance in degrees for a nearest neighbor.
            Default of None results in no upper bound on the distance.
        return_indices : `bool`, optional
            Return tuple of (idx, i1, i2, d) instead of (d, idx).
            Only supported with k=1.

        Returns
        -------
        d : array-like, float
            Array of distances in degrees.  Same shape as input array with
            axis of dimension `k` added to the end.  If `k=1` then this
            last dimension is squeezed out.
        idx : array-like, int
            Array of indices.  Same shape as input array with axis of
            dimension `k` added to the end.  If `k=1` then this last
            dimension is squeezed out.
        i1 : array-like, int
            Array of indices for matcher lon/lat.
            Returned if return_indices is True.
        i2 : array-like, int
            Array of indices for query lon/lat.
            Returned if return_indices is True.
        """
        if distance_upper_bound is not None:
            maxd = 2*np.sin(np.deg2rad(distance_upper_bound)/2.)
        else:
            maxd = np.inf

        if k != 1 and return_indices:
            raise NotImplementedError("Indices are only returned for 1-1 matches")

        coords = hp.rotator.dir2vec(lon, lat, lonlat=True).T
        d, idx = self.tree.query(coords, k=k, p=2, distance_upper_bound=maxd)

        with np.warnings.catch_warnings():
            np.warnings.simplefilter("ignore")
            np.arcsin(d/2, out=d)
        d = np.rad2deg(2*d)

        if return_indices:
            i2, = np.where(np.isfinite(d))
            i1 = idx[i2]
            return idx, i1, i2, d
        else:
            return d, idx

    def query_radius(self, lon, lat, radius, eps=0.0, return_indices=False):
        """Find all points in (lon, lat) that are within `radius` of the
        points in the matcher.

        Parameters
        ----------
        lon : array-like
            The longitude in degrees.
        lat : array-like
            The latitude in degrees.
        radius : `float`
            The match radius in degrees.
        eps : `float`, optional
            If non-zero, the set of returned points are correct to within a
            fraction precision of `eps` being closer than `radius`.
        return_indices : `bool`, optional
            Return tuple of (idx, i1, i2, distance) instead of just idx.

        Returns
        -------
        idx : `list` [`list` [`int`]]
            Each row in idx corresponds to each position in matcher lon/lat.
            The indices in the row correspond to the indices in query lon/lat.
        i1 : array-like, int
            Array of indices for matcher lon/lat.
            Returned if return_indices is True.
        i2 : array-like, int
            Array of indices for query lon/lat.
            Returned if return_indices is True.
        distance : array-like, float
            Array of distance (degrees) for each match pair.
            Returned if return_indices is True.
        """
        coords = hp.rotator.dir2vec(lon, lat, lonlat=True).T
        # The second tree in the match does not need to be balanced, and
        # turning this off yields significantly faster runtime.
        qtree = cKDTree(coords, compact_nodes=False, balanced_tree=False)
        angle = 2.0*np.sin(np.deg2rad(radius)/2.0)
        idx = self.tree.query_ball_tree(qtree, angle, eps=eps)

        if return_indices:
            n_match_per_obj = np.array([len(row) for row in idx])
            i1 = np.repeat(np.arange(len(idx)), n_match_per_obj)
            i2 = np.array(functools.reduce(operator.iconcat, idx, []))
            ds = esutil.coords.sphdist(
                self.lon[i1], self.lat[i1],
                lon[i2], lat[i2],
            )
            return idx, i1, i2, ds
        else:
            return idx

    def query_self(self, radius, min_match=1, eps=0.0, return_indices=False):
        """Match the list of lon/lat to itself.

        Parameters
        ----------
        radius : `float`
            The match radius in degrees.
        min_match : `int`, optional
            Minimum number of matches to count as a match.
            If min_match=1 then all positions will be returned since every
            position will match at least to itself.
        eps : `float`, optional
            If non-zero, the set of returned points are correct to within a
            fraction precision of `eps` being closer than `radius`.
        return_indices : `bool`, optional
            Return tuple of (idx, i1, i2, distance) instead of just idx.

        Returns
        -------
        idx : `list` [`list` [`int`]]
            Each row in idx corresponds to each position in matcher lon/lat.
            The indices in the row correspond to the indices in query lon/lat.
        i1 : array-like
            Array of indices for matcher lon/lat.
            Returned if return_indices is True.
        i2 : array-like
            Array of indices for query lon/lat.
            Returned if return_indices is True.
        distance : array-like
            Array of distance (degrees) for each match pair.
            Returned if return_indices is True.
        """
        angle = 2.0*np.sin(np.deg2rad(radius)/2.0)
        idx = self.tree.query_ball_tree(self.tree, angle, eps=eps)
        # The matching works best when sorting with the most matches first
        len_arr = np.array([len(j) for j in idx])
        st = np.argsort(len_arr)[:: -1]
        match_id = np.full(len(idx), -1, dtype=np.int32)
        idx2 = []
        for j in st:
            if match_id[j] < 0 and len_arr[j] >= min_match:
                match_id[idx[j]] = j
                idx2.append(idx[j])
        if return_indices:
            n_match_per_obj = np.array([len(row) for row in idx2])
            first_match_per_obj = np.array([row[0] for row in idx2])
            i1 = np.repeat(first_match_per_obj, n_match_per_obj)
            i2 = np.array(functools.reduce(operator.iconcat, idx2, []))
            # The distance is arbitrary here.
            ds = esutil.coords.sphdist(
                self.lon[i1], self.lat[i1],
                self.lon[i2], self.lat[i2],
            )
            return idx2, i1, i2, ds
        else:
            return idx2

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception_value, traceback):
        # Clear the memory from the tree
        del self.tree
