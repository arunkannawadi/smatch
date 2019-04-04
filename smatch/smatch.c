/*

    TODO:
        - optimizations
            - no need to have cat_ind in the match structure
              since it is implied by being in the catalog entry!
            - build heap on the fly rather than pushing then heapifying 
              when capacity is met
            - don't make the matches arrays until needed
            - test speed when sorting.  Currently sorting right before
            the copy to output array and right before writing to disk
*/

#include <Python.h>
#include <numpy/arrayobject.h> 

#include "math.h"
#include "defs.h"
#include "vector.h"
#include "tree.h"
#include "healpix.h"
#include "cat.h"

struct PySMatchCat {
    PyObject_HEAD
    int64_t maxmatch;
    int matching_self;

    Catalog *cat;
    struct healpix* hpix;
    struct tree_node* tree;

    // we keep this separately, for the case of writing
    // matches to a file
    int64_t nmatches;

    //match_vector *matches;

};

// sort function for the matches
/*
static int match_compare(const void *a, const void *b) {
    // we want to sort largest first, so will
    // reverse the normal trend
    double temp = 
        ((Match*)b)->cosdist
         -
        ((Match*)a)->cosdist;
    if (temp > 0)
        return 1;
    else if (temp < 0)
        return -1;
    else
        return 0;
}
*/

//
// sort a match vector using quicksort
//

/*
static void match_vector_sort(match_vector* self) {
    qsort(self->data, self->size, sizeof(Match), match_compare);
}
*/

/*
static void push_matches(match_vector* self, const match_vector* matches)
{
    size_t i=0;
    const Match* match=NULL;

    for (i=0; i<matches->size; i++) {
        match=&matches->data[i];

        vector_push(self, *match);
    }
    
}
*/

//
// build a heap in an existing match vector
//

static inline void match_build_heap(match_vector* self)
{
    size_t i=0, c=0, parent;
    Match* data = self->data;
    Match tmp;

    if (vector_size(self) <= 1) {
        // Its already a heap
        return;
    }

    for (i=1; i<vector_size(self)-1; i++) {
        c=i;
		do {
			parent  = (c - 1) / 2;
			if  (data[parent].cosdist < data[c].cosdist)
			{
				tmp =  data[parent];
				data[parent] = data[c];
				data[c]  = tmp;
			}
			c =  parent;
		} while (c !=  0);
    }
}

//
//   make sure the heap is heapified after putting a new element
//   at the root
//
//   it is assumed the data after the root already form a heap
//

static void match_heap_fix(match_vector* self)
{
    size_t n = vector_size(self)-1;

    const Match* v = &self->data[0];
    Match* data = self->data;

    size_t jhi = 0;
    size_t jlo = 1;

    while (jlo <= n) {
        if (jlo < n && data[jlo].cosdist > data[jlo+1].cosdist) {
            // The right node is smaller
            jlo += 1;
        }
        if (v->cosdist <= data[jlo].cosdist) {
            // It forms a heap already
            break;
        }

        data[jhi] = data[jlo]; // promotes the smaller of the branches
        jhi = jlo;             // move down the heap
        jlo = 2*jlo + 1;       // calculates position of left branch
    }

    data[jhi] = *v; // places v, vind at correct position in heap

}

//
//    possibly insert value, displacing larger values
//    it is asssumed the data are already a heap other than
//    the first item
//

static inline void match_heap_insert(match_vector* self, const Match* match)
{
    if (match->cosdist > self->data[0].cosdist) {
        self->data[0] = *match;
        if (vector_size(self) > 1) {
            match_heap_fix(self);
        }
    }
}

//
// We assume these are in degrees, double precision, and are numpy arrays of
// same length
//
// radii get converted to radians
//

static Catalog* catalog_init(PyObject* raObj,
                             PyObject* decObj,
                             PyObject* radObj,
                             struct healpix* hpix) {
    int status=0;

    Catalog *cat=NULL;
    CatalogEntry *entry=NULL;
    Point* pt = NULL;
    double *raptr=NULL, *decptr=NULL, *radptr=NULL;

    npy_intp n=0, i=0, nrad=0;
    n = PyArray_SIZE(raObj);
    if (n <= 0) {
        PyErr_SetString(PyExc_ValueError, "Entered ra/dec must have size > 0\n");
        goto _catalog_init_bail;
    }
    nrad = PyArray_SIZE(radObj);
    if (nrad != n) {
        PyErr_Format(PyExc_ValueError, 
                     "radii must be same length as ra,dec (%ld).  Got %ld\n",n,nrad);
        goto _catalog_init_bail;
    }

    // this sets matches in each element to a new match_vector
    cat = cat_new(n);

    for (i=0; i<n; i++) {

        entry = &cat->data[i];

        pt=&entry->point;

        raptr=PyArray_GETPTR1(raObj, i);
        decptr=PyArray_GETPTR1(decObj, i);

        status=hpix_eq2xyz(*raptr, *decptr, &pt->x, &pt->y, &pt->z);
        if (!status) {
            // we expect the error to be set already
            goto _catalog_init_bail;
        }

        radptr = PyArray_GETPTR1(radObj, i);
        pt->radius = (*radptr)*D2R;
        pt->cos_radius = cos( pt->radius );

        hpix_disc_intersect(hpix, pt->x, pt->y, pt->z, pt->radius, entry->disc_pixels);

    }

_catalog_init_bail:

    if (!status && cat) {
        // cat set to NULL
        cat_free(cat);
    }
    return cat;
}

//
// create the tree
// for each point and radius, the associated disk is intersected
// and the catalog entry is inserted into the tree for matches
//

/*
static struct tree_node* create_tree(struct healpix* hpix, Catalog* cat) {
    struct tree_node* tree=NULL;
    lvector* listpix=NULL;
    Point* pt=NULL;
    int64_t hpixid=0;
    int64_t half_npix=0;
    size_t index=0, ihpix=0;
    
    listpix = lvector_new();

    // this will produce a more balanced tree across the whole sky
    half_npix = hpix->npix/2;

    for (index=0; index < cat->size; index++) {

        pt = &cat->data[index].point;

        hpix_disc_intersect(hpix, pt->x, pt->y, pt->z, pt->radius, listpix);

        for (ihpix=0; ihpix < listpix->size; ihpix++) {
            hpixid = listpix->data[ihpix];
            tree_insert(&tree, hpixid-half_npix, index);
        }
    }
    vector_free(listpix);

    return tree;
}
*/

static struct tree_node* create_hpix_tree(struct healpix* hpix,
                                          PyObject* raObj,
                                          PyObject* decObj,
                                          int *status)
{

    struct tree_node* tree=NULL;
    int64_t hpixid=0;
    int64_t half_npix=0;
    size_t i=0, n=0;
    double *raptr=NULL, *decptr=NULL;
    
    // this will produce a more balanced tree across the whole sky
    half_npix = hpix->npix/2;

    n = PyArray_SIZE(raObj);
    for (i=0; i<n ; i++) {

        raptr=PyArray_GETPTR1(raObj, i);
        decptr=PyArray_GETPTR1(decObj, i);

        hpixid = hpix_eq2pix(hpix, *raptr, *decptr, status);
        if ( !(*status) ) {
            PyErr_SetString(PyExc_ValueError, "Could not get hpix id, band ra,dec\n");
            goto _create_hpix_tree_bail;
        }

        tree_insert(&tree, hpixid-half_npix, i);

    }

_create_hpix_tree_bail:

    if ( !(*status) ) {
        tree = tree_delete(tree);
    }

    return tree;
}


//
// initialize the python catalog object
//

static int
PySMatchCat_init(struct PySMatchCat* self, PyObject *args, PyObject *kwds)
{
    PY_LONG_LONG nside=0;
    PyObject* raObj=NULL;
    PyObject* decObj=NULL;
    PyObject* radObj=NULL;
    int err=0;

    if (!PyArg_ParseTuple(args, (char*)"LOOO", &nside, &raObj, &decObj, &radObj)) {
        return -1;
    }

    self->tree=NULL;
    self->hpix=NULL;

    //self->matches = match_vector_new();

    self->hpix = hpix_new((int64_t)nside);
    if (self->hpix==NULL) {
        err=1;
        goto _catalog_init_cleanup;
    }

    self->cat = catalog_init(raObj, decObj, radObj, self->hpix);
    if (self->cat==NULL) {
        err=1;
        goto _catalog_init_cleanup;
    }

    //self->tree = create_tree(self->hpix, self->cat);
    //if (self->tree==NULL) {
    //    err=1;
    //    goto _catalog_init_cleanup;
    //}

_catalog_init_cleanup:
    if (err != 0) {
        cat_free(self->cat);
        self->hpix = hpix_delete(self->hpix);
        //self->tree = tree_delete(self->tree);
        return -1;
    }
    return 0;
}


//
// deallocate the python object
//

static void
PySMatchCat_dealloc(struct PySMatchCat* self)
{

    cat_free(self->cat);
    self->hpix = hpix_delete(self->hpix);
    //self->tree = tree_delete(self->tree);
    //vector_free(self->matches);

#if PY_MAJOR_VERSION >= 3
    Py_TYPE(self)->tp_free((PyObject*)self);
#else
    self->ob_type->tp_free((PyObject*)self);
#endif

}

//
// a repr for the python object
//

static PyObject *
PySMatchCat_repr(struct PySMatchCat* self) {
    char repr[256];
    sprintf(repr, "Catalog\n    hpix nside: %ld", self->hpix->nside);
#if PY_MAJOR_VERSION >= 3
    // bytes
    return Py_BuildValue("y",repr);
#else
    return Py_BuildValue("s",repr);
#endif
}

//
// getters
//

static PyObject *
PySMatchCat_hpix_nside(struct PySMatchCat* self) {
    return Py_BuildValue("l", self->hpix->nside);
}
static PyObject *
PySMatchCat_hpix_area(struct PySMatchCat* self) {
    return Py_BuildValue("d", hpix_area(self->hpix->nside));
}

static PyObject *
PySMatchCat_nmatches(struct PySMatchCat* self) {
    return Py_BuildValue("l", self->nmatches);
}

/*
static PyObject* PySMatchCat_set_nmatches(struct PySMatchCat* self, PyObject *args)
{
    PY_LONG_LONG nmatches=0;

    if (!PyArg_ParseTuple(args, (char*)"L", &nmatches)) {
        return NULL;
    }

    self->nmatches=nmatches;
    Py_RETURN_NONE;
}
*/

/*

   Match the input ra,dec to the catalog. If no restriction is set on maximum
   number of matches, then matches are simply appended.

   If the number of matches is restricted (maxmatch > 0) then matches are
   appended up to the max allowed, then the match vector is converted to a heap
   and only matches closer than the farthest current match are added.

   parameters

   self: the catalog
   ra, dec: the point to match from the second catalog
   input_ind: the index in the second catalog
   match_incr: this gets filled with the increase
     in match count.  If a new match is found and
     replaces an older farther match, the match
     count is not incremented.

*/

static int domatch1(struct PySMatchCat* self, 
                    struct tree_node* tree,
                    size_t cat_ind,
                    PyObject* raObj,
                    PyObject* decObj)
{
    int status=0;

    double *raptr=NULL, *decptr=NULL;
    CatalogEntry* entry=NULL;
    Point *pt=NULL;

    int64_t hpixid=0;
    struct tree_node* node=NULL;
    int64_t half_npix=0;

    size_t i=0, j=0, input_ind=0;
    double x=0,y=0,z=0;
    double cos_angle=0;

    int64_t maxmatch = self->maxmatch;

    Match match={0};
    match_vector* matches=NULL;

    half_npix = self->hpix->npix/2;

    entry = &self->cat->data[cat_ind];

    // loop over pixels that intersected a disc around
    // this object

    for (i=0; i < vector_size(entry->disc_pixels); i++) {

        // get the tree node corresponding to this pixel
        hpixid = vector_get(entry->disc_pixels, i);
        node = tree_find(tree, hpixid-half_npix);

        if (node != NULL) {
            for (j=0; j < vector_size(node->indices); j++) {

                input_ind = vector_get(node->indices, j);

                raptr=PyArray_GETPTR1(raObj, input_ind);
                decptr=PyArray_GETPTR1(decObj, input_ind);

                status=hpix_eq2xyz(*raptr, *decptr, &x, &y, &z);
                if (!status) {
                    // we expect the error to be set already
                    goto _domatch1_bail;
                }

                pt = &entry->point;
                cos_angle = pt->x*x + pt->y*y + pt->z*z;

                if (cos_angle > pt->cos_radius) {
                    match.cat_ind=cat_ind;
                    match.input_ind=(int64_t)input_ind;
                    match.cosdist=cos_angle;

                    matches = entry->matches;

                    if (maxmatch <= 0 || (int64_t)vector_size(matches) < maxmatch) {
                        // we increment for new matches
                        self->nmatches += 1;

                        // just keep adding entries
                        vector_push(matches, match);

                        // if we are now at capacity, heapify it unless maxmatch
                        // is size one, in which case it is already a heap
                        if (maxmatch > 1 && (int64_t)vector_size(matches)==maxmatch) {
                            match_build_heap(matches);
                        }
                    } else {
                        // note the number of matches is *not* incremented

                        // add only if closer than the farthest match
                        match_heap_insert(matches, &match);
                    }

                } // within distance
            } // loop over indices in node
        } // id found in tree
    } // loop over disc pixel ids

    status=1;

_domatch1_bail:
    return status;
}

//
// do the match for each entred point
//

static int domatch(struct PySMatchCat* self,
                   PyObject* raObj,
                   PyObject* decObj) {
    int status=0;

    size_t i=0;

    struct tree_node* tree=NULL;

    tree = create_hpix_tree(self->hpix, raObj, decObj, &status);
    if (!status) {
        goto _domatch_bail;
    }

    self->nmatches=0;

    for (i=0; i< self->cat->size ; i++) {

        status = domatch1(self, tree, i, raObj, decObj);
        if (!status) {
            goto _domatch_bail;
        }

    }
    tree = tree_delete(tree);

_domatch_bail:
    return status;
}


/*
static int domatch1_old(struct PySMatchCat* self, 
                    double ra,
                    double dec,
                    size_t input_ind,
                    size_t* match_incr) {

    int status=0;

    CatalogEntry* entry=NULL;
    Point* pt=NULL;

    int64_t hpixid=0;
    struct tree_node* node=NULL;
    int64_t half_npix=0;

    size_t i=0;
    int64_t cat_ind=0;
    double x=0,y=0,z=0;
    double cos_angle=0;

    Match match={0};
    match_vector* matches=NULL;

    *match_incr = 0;

    half_npix = self->hpix->npix/2;
    hpixid = hpix_eq2pix(self->hpix, ra, dec, &status);
    if (!status) {
        goto _domatch1_bail;
    }

    hpixid -= half_npix;

    node = tree_find(self->tree, hpixid);

    if (node != NULL) {


        status=hpix_eq2xyz(ra,dec,&x,&y,&z);
        if (!status) {
            goto _domatch1_bail;
        }

        for (i=0; i<node->indices->size; i++) {
            // index into other list
            cat_ind = node->indices->data[i];

            if (self->matching_self && cat_ind == input_ind) {
                continue;
            }

            entry = &self->cat->data[cat_ind];
            pt = &entry->point;

            cos_angle = pt->x*x + pt->y*y + pt->z*z;

            if (cos_angle > pt->cos_radius) {
                match.cat_ind=cat_ind;
                match.input_ind=(int64_t)input_ind;
                match.cosdist=cos_angle;

                matches = entry->matches;

                if (self->maxmatch <= 0 || vector_size(matches) < self->maxmatch) {
                    // we increment for new matches
                    *match_incr += 1;
                    // just keep adding entries
                    vector_push(matches, match);

                    // if we are now at capacity, heapify it unless maxmatch
                    // is size one, in which case it is already a heap
                    if (self->maxmatch > 1 && vector_size(matches)==self->maxmatch) {
                        match_build_heap(matches);
                    }
                } else {
                    // note the number of matches is *not* incremented

                    // add only if closer than the farthest match
                    match_heap_insert(matches, &match);
                }
            }
        }
    }

_domatch1_bail:
    return status;
}


static int domatch_old(struct PySMatchCat* self,
                   PyObject* raObj,
                   PyObject* decObj) {
    int status=0;

    size_t i=0, n=0, match_incr=0;
    double *raptr=NULL, *decptr=NULL;

    self->nmatches=0;

    n = PyArray_SIZE(raObj);
    for (i=0; i<n ; i++) {

        raptr=PyArray_GETPTR1(raObj, i);
        decptr=PyArray_GETPTR1(decObj, i);

        status=domatch1(self, *raptr, *decptr, i, &match_incr);
        if (!status) {
            goto _domatch_bail;
        }

        self->nmatches += match_incr;

    }

_domatch_bail:
    return status;
}
*/


//
// Prepare for matching.  We clear the match vectors, possibly
// reallocating or getting a maxmatch sized zerod vector
//

static void match_prep(struct PySMatchCat* self)
{
    size_t i=0;

    Catalog* cat=self->cat;
    CatalogEntry* entry=NULL;

    for (i=0; i< cat->size; i++) {
        entry = &cat->data[i];
        
        if (self->maxmatch <= 0) {
            // we will not restrict matches, just reset the capacity
            // to begin with minimal memory usage. If the capacity
            // is already small, just resize.  We will then
            // simply push all matches
            if (vector_capacity(entry->matches) > 1) {
                vector_clear(entry->matches);
            } else {
                vector_resize(entry->matches,0);
            }
        } else {
            vector_realloc(entry->matches, self->maxmatch);
            vector_resize(entry->matches, 0);
        }
    }
}

//
// do the matching, filling in the match vectors for each point
// as we go
//

static PyObject* PySMatchCat_match(struct PySMatchCat* self, PyObject *args)
{
    int status=0;
    PyObject* raObj=NULL;
    PyObject* decObj=NULL;

    if (!PyArg_ParseTuple(args, (char*)"LiOO",
                          &self->maxmatch,
                          &self->matching_self,
                          &raObj,
                          &decObj)) {
        return NULL;
    }

    match_prep(self);
    status=domatch(self, raObj, decObj);

    if (!status) {
        return NULL;
    } else {
        Py_RETURN_NONE;
    }
}

/*

   send matches to a file for the case of not limiting matches

   don't call this function if you are limiting matches

*/
static int domatch1_2file(struct PySMatchCat* self, 
                          double ra,
                          double dec,
                          size_t input_ind,
                          FILE* fobj,
                          size_t* match_incr) {

    int status=0, nret=0;

    CatalogEntry* entry=NULL;
    Point* pt=NULL;

    int64_t hpixid=0;
    struct tree_node* node=NULL;
    int64_t half_npix=0;

    size_t i=0;
    int64_t cat_ind=0;
    double x=0,y=0,z=0;
    double cos_angle=0;

    *match_incr = 0;

    half_npix = self->hpix->npix/2;
    hpixid = hpix_eq2pix(self->hpix, ra, dec, &status);
    if (!status) {
        goto _domatch1_2file_bail;
    }

    hpixid -= half_npix;

    node = tree_find(self->tree, hpixid);

    if (node != NULL) {


        status=hpix_eq2xyz(ra,dec,&x,&y,&z);
        if (!status) {
            goto _domatch1_2file_bail;
        }

        for (i=0; i<node->indices->size; i++) {
            // index into other list
            cat_ind = node->indices->data[i];

            if (self->matching_self && cat_ind == (int64_t)input_ind) {
                continue;
            }

            entry = &self->cat->data[cat_ind];
            pt = &entry->point;

            cos_angle = pt->x*x + pt->y*y + pt->z*z;

            if (cos_angle > pt->cos_radius) {
                *match_incr += 1;
                nret = fprintf(fobj, "%ld %ld %.16g\n", cat_ind, input_ind, cos_angle);
                if (nret == 0) {
                    status = 0;
                    goto _domatch1_2file_bail;
                }
            }
        }
    }

    status=1;

_domatch1_2file_bail:
    return status;
}

static int domatch2file_all(struct PySMatchCat* self,
                            PyObject* raObj,
                            PyObject* decObj,
                            FILE* fobj) {

    int status=0;
    size_t i=0, n=0, match_incr=0;
    double *raptr=NULL, *decptr=NULL;

    n = PyArray_SIZE(raObj);
    for (i=0; i<n ; i++) {

        raptr=PyArray_GETPTR1(raObj, i);
        decptr=PyArray_GETPTR1(decObj, i);

        status=domatch1_2file(self, *raptr, *decptr, i, fobj, &match_incr);
        if (!status) {
            goto _domatch2fileall_bail;
        }

        self->nmatches += match_incr;

    }

_domatch2fileall_bail:

    return status;

}

static int write_matches(struct PySMatchCat* self, FILE *fobj)
{
    size_t i=0, j=0;
    match_vector *matches=NULL;
    Match* match=NULL;
    int status=0, nret=0;

    for (i=0; i<self->cat->size; i++) {
        matches = self->cat->data[i].matches;
        //match_vector_sort(matches);

        for (j=0; j<vector_size(matches); j++) {
            match = &vector_get(matches, j);
            nret = fprintf(fobj, "%ld %ld %.16g\n",
                           match->cat_ind, match->input_ind, match->cosdist);
            if (nret == 0) {
                status=0;
                goto _write_matches_bail;
            }
        }
    }

    status=1;

_write_matches_bail:
    return status;
}

static int domatch2file(struct PySMatchCat* self,
                        PyObject* raObj,
                        PyObject* decObj,
                        const char* filename) {
    int status=0;
    FILE* fobj=NULL;

    fobj=fopen(filename, "w");
    if (fobj == NULL) {
        PyErr_Format(PyExc_IOError, "Could not open file for writing: '%s'", filename);
        goto _domatch2file_bail;
    }

    if (self->maxmatch <= 0) {
        // keeping all matches
        status = domatch2file_all(self, raObj, decObj,fobj);
    } else {
        // do all matches, *then* write to the file
        match_prep(self);
        status = domatch(self, raObj, decObj);
        if (!status) {
            goto _domatch2file_bail;
        }
        status = write_matches(self, fobj);
    }


_domatch2file_bail:

    if (fobj) {
        fclose(fobj);
    }

    return status;
}

/*

   do the matching and write to the indicated file

*/
static PyObject* PySMatchCat_match2file(struct PySMatchCat* self, PyObject *args)
{
    int status=0;
    PyObject* raObj=NULL;
    PyObject* decObj=NULL;
    const char *filename=NULL;

    if (!PyArg_ParseTuple(args, (char*)"LiOOs",
                          &self->maxmatch,
                          &self->matching_self,
                          &raObj,
                          &decObj,
                          &filename)) {
        return NULL;
    }

    status=domatch2file(self, raObj, decObj, filename);

    if (!status) {
        return NULL;
    } else {
        Py_RETURN_NONE;
    }

}

int64_t count_lines(FILE* fobj)
{

    char ch;
	int64_t nlines=0;

	while(!feof(fobj))
	{
		ch = fgetc(fobj);
		if(ch == '\n')
		{
			nlines++;
		}
	}

	return nlines;
}

static PyObject *
PySMatchCat_count_lines(PyObject* self, PyObject* args)
{
    const char *filename=NULL;
    FILE* fobj=NULL;
    int64_t nlines=0;

    if (!PyArg_ParseTuple(args, (char*)"s", &filename)) {
        return NULL;
    }

    fobj=fopen(filename, "r");
    if (fobj == NULL) {
        PyErr_Format(PyExc_IOError, "Could not open file: '%s'\n", filename);
        return NULL;
    }

    nlines = count_lines(fobj);

    fclose(fobj);

    return Py_BuildValue("l", nlines);

}

/*

   read from a match file

   i1 i2 cosdist

   i1->catalog index
   i2->secondary or input index for the match2file routine
   cosdist-> cos(angular distance)

*/

static PyObject* PySMatchCat_load_matches(PyObject* self, PyObject *args)
{
    const char *filename=NULL;
    FILE* fobj=NULL;
    int nread=0, ncols=3;
    npy_intp nmatches=0, i=0;
    Match *match=NULL;

    PyObject *matchesObj=NULL;

    if (!PyArg_ParseTuple(args, (char*)"sO", &filename, &matchesObj)) {
        return NULL;
    }
    nmatches = PyArray_SIZE(matchesObj);

    if (nmatches <= 0) {
        // nothing to do
        Py_RETURN_NONE;
    }

    fobj=fopen(filename, "r");
    if (fobj == NULL) {
        PyErr_Format(PyExc_IOError, "Could not open file: '%s'\n", filename);
        return NULL;
    }

    for (i=0; i<nmatches; i++) {
        match = (Match* ) PyArray_GETPTR1(matchesObj, i);
        nread=fscanf(fobj,
                     "%ld %ld %lf\n", 
                     &match->cat_ind,
                     &match->input_ind,
                     &match->cosdist);
        if (nread != ncols) {
            PyErr_Format(PyExc_IOError,
                         "Error: only read %d at line %ld of file: '%s'\n", nread,
                         i+1,
                         filename);
            goto _load_matches_bail;
        }
    }

_load_matches_bail:
    fclose(fobj);
    if (nread != ncols) {
        return NULL;
    }

    Py_RETURN_NONE;
}



/*
   Copy into the given structured array.

   No error checking is performed here, set up the data in python

   match vectors are freed as we go

*/
static PyObject* PySMatchCat_copy_matches(struct PySMatchCat* self, PyObject *args)
{
    PyObject* matchesObj=NULL;
    Match* matches=NULL;
    match_vector *tmatches=NULL;
    size_t i=0, mindex=0, nmatch=0;

    if (!PyArg_ParseTuple(args, (char*)"O", &matchesObj)) {
        return NULL;
    }

    matches = PyArray_DATA(matchesObj);

    for (i=0; i< self->cat->size; i++) {
        tmatches = self->cat->data[i].matches;

        nmatch = vector_size(tmatches);
        if (nmatch > 0) {

            // we can traverse heap in order rather than doing this, but not sure
            // which is faster
            // this can be a hit in performance when the match vectors
            // get large, although not a huge one.  The main reason I'm avoiding
            // it is because it is not possible to do this sorting for
            // the case of writing to a file without restriction on the number
            // of matches, so this would be inconsistent
            //match_vector_sort(tmatches);

            memmove(&matches[mindex],
                    tmatches->data,
                    nmatch*sizeof(Match) );
            mindex += nmatch;
        }

        // clear the memory for match structures larger than 1, otherise
        // just set the visible size to 0
        if (vector_capacity(tmatches) > 1) {
            vector_clear(tmatches);
        } else {
            vector_resize(tmatches, 0);
        }

    }

    Py_RETURN_NONE;
}

static PyMethodDef PySMatchCat_methods[] = {
    {"get_nmatches",           (PyCFunction)PySMatchCat_nmatches,          METH_VARARGS,  "Get the number of matches."},
    //{"_set_nmatches",           (PyCFunction)PySMatchCat_set_nmatches,          METH_VARARGS,  "Set the number of matches, useful when reading from a file."},
    {"get_hpix_nside",              (PyCFunction)PySMatchCat_hpix_nside,          METH_VARARGS,  "Get the nside for healpix."},
    {"get_hpix_area",              (PyCFunction)PySMatchCat_hpix_area,          METH_VARARGS,  "Get the nside for healpix."},
    {"match",              (PyCFunction)PySMatchCat_match,          METH_VARARGS,  "Match the catalog to the input ra,dec arrays."},
    {"match2file",              (PyCFunction)PySMatchCat_match2file,          METH_VARARGS,  "Match the catalog to the input ra,dec arrays and write results to a file."},
    {"_copy_matches",              (PyCFunction)PySMatchCat_copy_matches,          METH_VARARGS,  "Copy the matches into the input array."},
    {NULL}  /* Sentinel */
};



static PyTypeObject PyCatalogType = {
#if PY_MAJOR_VERSION >= 3
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
#endif
    "_smatch.Catalog",             /*tp_name*/
    sizeof(struct PySMatchCat), /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)PySMatchCat_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    //0,                         /*tp_repr*/
    (reprfunc)PySMatchCat_repr,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "Catalog Class",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    PySMatchCat_methods,             /* tp_methods */
    0,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    //0,     /* tp_init */
    (initproc)PySMatchCat_init,      /* tp_init */
    0,                         /* tp_alloc */
    PyType_GenericNew,                 /* tp_new */
};







static PyMethodDef smatch_module_methods[] = {
    {"_count_lines",      (PyCFunction)PySMatchCat_count_lines, METH_VARARGS,  "count the lines in the specified file."},
    {"_load_matches",              (PyCFunction)PySMatchCat_load_matches,          METH_VARARGS,  "Load matches from the specifed filename."},
};


#if PY_MAJOR_VERSION >= 3
    static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        "_smatch",      /* m_name */
        "Defines the catalog class and some methods",  /* m_doc */
        -1,                  /* m_size */
        smatch_module_methods, /* m_methods */
        NULL,                /* m_reload */
        NULL,                /* m_traverse */
        NULL,                /* m_clear */
        NULL,                /* m_free */
    };
#endif



#ifndef PyMODINIT_FUNC  /* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif

PyMODINIT_FUNC
#if PY_MAJOR_VERSION >= 3
PyInit__smatch(void) 
#else
init_smatch(void) 
#endif
{
    PyObject* m;

    PyCatalogType.tp_new = PyType_GenericNew;


#if PY_MAJOR_VERSION >= 3
    if (PyType_Ready(&PyCatalogType) < 0) {
        return NULL;
    }
    m = PyModule_Create(&moduledef);
    if (m==NULL) {
        return NULL;
    }

#else

    if (PyType_Ready(&PyCatalogType) < 0)
        return;

    m = Py_InitModule3("_smatch", smatch_module_methods, "Define module methods.");
    if (m==NULL) {
        return;
    }
#endif

    Py_INCREF(&PyCatalogType);
    PyModule_AddObject(m, "Catalog", (PyObject *)&PyCatalogType);

    import_array();

#if PY_MAJOR_VERSION >= 3
    return m;
#endif
}
