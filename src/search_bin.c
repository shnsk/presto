#include "presto.h"
#include "search_bin_cmd.h"

/* The number of candidates to return from the search of each miniFFT */
#define MININCANDS 6

/* Minimum binary period (s) to accept as 'real' */
#define MINORBP 300.0

/* Minimum miniFFT bin number to accept as 'real' */
#define MINMINIBIN 3.0

/* Bins to ignore at the beginning and end of the big FFT */
#define BINSTOIGNORE 0

/* Function definitions */
int not_already_there_rawbin(rawbincand newcand, 
 			    rawbincand *list, int nlist);
int comp_rawbin_to_cand(rawbincand *cand, infodata * idata,
 		       char *output, int full);
void compare_rawbin_cands(rawbincand *list, int nlist, 
 			 char *notes);
void file_rawbin_candidates(rawbincand *cand, char *notes,
				   int numcands, char name[]);
float percolate_rawbincands(rawbincand *cands, int numcands);

/* Main routine */

#ifdef USEDMALLOC
#include "dmalloc.h"
#endif

int main(int argc, char *argv[])
{
  FILE *fftfile, *candfile;
  float powargr, powargi, *powers = NULL, *minifft;
  float norm, numchunks, *powers_pos;
  int nbins, newncand, nfftsizes, fftlen, halffftlen, binsleft;
  int numtoread, filepos = 0, loopct = 0, powers_offset, ncand2;
  int ii, ct, newper = 0, oldper = 0, numsumpow = 1;
  double T, totnumsearched = 0.0, minsig = 0.0;
  char filenm[200], candnm[200], binnm[200], *notes;
  fcomplex *data = NULL;
  rawbincand tmplist[MININCANDS], *list;
  infodata idata;
  struct tms runtimes;
  double ttim, utim, stim, tott;
  Cmdline *cmd;

  /* Prep the timer */

  tott = times(&runtimes) / (double) CLK_TCK;

  /* Call usage() if we have no command line arguments */

  if (argc == 1) {
    Program = argv[0];
    printf("\n");
    usage();
    exit(1);
  }

  /* Parse the command line using the excellent program Clig */

  cmd = parseCmdline(argc, argv);

#ifdef DEBUG
  showOptionValues();
#endif

  printf("\n\n");
  printf("          Binary Pulsar Search Routine\n");
  printf("              by Scott M. Ransom\n");
  printf("                 23 Sept, 1999\n\n");

  /* Initialize the filenames: */

  sprintf(filenm, "%s.fft", cmd->argv[0]);
  sprintf(candnm, "%s_bin.cand", cmd->argv[0]);
  sprintf(binnm, "%s_bin", cmd->argv[0]);

  /* Read the info file */

  readinf(&idata, cmd->argv[0]);
  T = idata.N * idata.dt;
  if (idata.object) {
    printf("Analyzing %s data from '%s'.\n\n", idata.object, filenm);
  } else {
    printf("Analyzing data from '%s'.\n\n", filenm);
  }

  /* open the FFT file and get its length */

  fftfile = chkfopen(filenm, "rb");
  nbins = chkfilelen(fftfile, sizeof(fcomplex));

  /* Check that cmd->maxfft is an acceptable power of 2 */

  ct = 4;
  ii = 1;
  while (ct < MAXREALFFT || ii) {
    if (ct == cmd->maxfft)
      ii = 0;
    ct <<= 1;
  }
  if (ii) {
    printf("\n'maxfft' is out of range or not a power-of-2.\n\n");
    exit(1);
  }

  /* Check that cmd->minfft is an acceptable power of 2 */

  ct = 4;
  ii = 1;
  while (ct < MAXREALFFT || ii) {
    if (ct == cmd->minfft)
      ii = 0;
    ct <<= 1;
  }
  if (ii) {
    printf("\n'minfft' is out of range or not a power-of-2.\n\n");
    exit(1);
  }

  /* Low and high Fourier freqs to check */

  if (!cmd->rloP || (cmd->rloP && cmd->rlo < cmd->lobin)){
    if (cmd->lobin == 0)
      cmd->rlo = BINSTOIGNORE;
    else
      cmd->rlo = cmd->lobin;
  }
  if (cmd->rhiP){
    if (cmd->rhi < cmd->rlo) cmd->rhi = cmd->rlo + cmd->maxfft;
    if (cmd->rhi > cmd->lobin + nbins) 
      cmd->rhi = cmd->lobin + nbins - BINSTOIGNORE;
  } else {
    cmd->rhi = cmd->lobin + nbins - BINSTOIGNORE;
  }

  /* Determine how many different mini-fft sizes we will use */

  nfftsizes = 1;
  ii = cmd->maxfft;
  while (ii > cmd->minfft) {
    ii >>= 1;
    nfftsizes++;
  }

  /* Allocate some memory and prep some variables.             */
  /* For numtoread, the 6 just lets us read extra data at once */

  numtoread = 6 * cmd->maxfft;
  if (cmd->stack == 0)
    powers = gen_fvect(numtoread);
  minifft = gen_fvect(cmd->maxfft);
  ncand2 = 2 * cmd->ncand;
  list = (rawbincand *)malloc(sizeof(rawbincand) * ncand2);
  for (ii = 0; ii < ncand2; ii++)
    list[ii].mini_sigma = 0.0;
  for (ii = 0; ii < MININCANDS; ii++)
    tmplist[ii].mini_sigma = 0.0;
  filepos = cmd->rlo - cmd->lobin;
  numchunks = (float) (cmd->rhi - cmd->rlo) / numtoread;
  printf("Searching...\n");
  printf("   Amount complete = %3d%%", 0);
  fflush(stdout);

  /* Loop through fftfile */

  while ((filepos + cmd->lobin) < cmd->rhi) {

    /* Calculate percentage complete */

    newper = (int) (loopct / numchunks * 100.0);

    if (newper > oldper) {
      newper = (newper > 99) ? 100 : newper;
      printf("\r   Amount complete = %3d%%", newper);
      oldper = newper;
      fflush(stdout);
    }

    /* Adjust our search parameters if close to end of zone to search */

    binsleft = cmd->rhi - (filepos + cmd->lobin);
    if (binsleft < cmd->minfft) 
      break;
    if (binsleft < numtoread) {  /* Change numtoread */
      numtoread = cmd->maxfft;
      while (binsleft < numtoread){
	cmd->maxfft /= 2;
	numtoread = cmd->maxfft;
      }
    }
    fftlen = cmd->maxfft;

    /* Read from fftfile */

    if (cmd->stack == 0){
      data = read_fcomplex_file(fftfile, filepos, numtoread);
      for (ii = 0; ii < numtoread; ii++)
	powers[ii] = POWER(data[ii].r, data[ii].i);
      numsumpow = 1;
    } else {
      powers = read_float_file(fftfile, filepos, numtoread);
      numsumpow = cmd->stack;
    }
    if (filepos == 0) powers[0] = 1.0;
      
    /* Chop the powers that are way above the median level */

    prune_powers(powers, numtoread, numsumpow);

    /* Loop through the different small FFT sizes */

    while (fftlen >= cmd->minfft) {

      halffftlen = fftlen / 2;
      powers_pos = powers;
      powers_offset = 0;

      /* Perform miniffts at each section of the powers array */

      while ((numtoread - powers_offset) >
	     (int) ((1.0 - cmd->overlap) * cmd->maxfft + DBLCORRECT)){

	/* Copy the proper amount and portion of powers into minifft */

	memcpy(minifft, powers_pos, fftlen * sizeof(float));

	/* Perform the minifft */

	realfft(minifft, fftlen, -1);

	/* Normalize and search the miniFFT */

	norm = sqrt((double) fftlen * (double) numsumpow) / minifft[0];
	for (ii = 0; ii < fftlen; ii++) minifft[ii] *= norm;
	search_minifft((fcomplex *)minifft, halffftlen, tmplist, \
		       MININCANDS, cmd->harmsum, cmd->numbetween, idata.N, \
		       T, (double) (powers_offset + filepos + cmd->lobin), \
		       cmd->interbinP ? INTERBIN : INTERPOLATE, \
		       cmd->noaliasP ? NO_CHECK_ALIASED : CHECK_ALIASED);
		       
	/* Check if the new cands should go into the master cand list */

	for (ii = 0; ii < MININCANDS; ii++){
	  if (tmplist[ii].mini_sigma > minsig) {

	    /* Insure the candidate is semi-realistic */

	    if (tmplist[ii].orb_p > MINORBP && 
		tmplist[ii].mini_r > MINMINIBIN &&
		tmplist[ii].mini_r < tmplist[ii].mini_N - MINMINIBIN &&
		fabs(tmplist[ii].mini_r - 0.5 * tmplist[ii].mini_N) > 2.0){

	      /* Check to see if another candidate with these properties */
	      /* is already in the list.                                 */
	      
	      if (not_already_there_rawbin(tmplist[ii], list, ncand2)) {
		list[ncand2 - 1] = tmplist[ii];
		minsig = percolate_rawbincands(list, ncand2);
	      }
	    } else {
	      continue;
	    }
	  } else {
	    break;
	  }
	  /* Mini-fft search for loop */
	}

	totnumsearched += fftlen;
	powers_pos += (int)(cmd->overlap * fftlen);
	powers_offset = powers_pos - powers;

	/* Position of mini-fft in data set while loop */
      }

      fftlen >>= 1;

      /* Size of mini-fft while loop */
    }

    if (cmd->stack == 0) free(data);
    else free(powers);
    filepos += (numtoread - (int)((1.0 - cmd->overlap) * cmd->maxfft));
    loopct++;

    /* File position while loop */
  }

  /* Print the final percentage update */

  printf("\r   Amount complete = %3d%%\n\n", 100);

  /* Print the number of frequencies searched */

  printf("Searched %.0f pts (including interbins).\n\n", totnumsearched);

  printf("Timing summary:\n");
  tott = times(&runtimes) / (double) CLK_TCK - tott;
  utim = runtimes.tms_utime / (double) CLK_TCK;
  stim = runtimes.tms_stime / (double) CLK_TCK;
  ttim = utim + stim;
  printf("    CPU time: %.3f sec (User: %.3f sec, System: %.3f sec)\n", \
	 ttim, utim, stim);
  printf("  Total time: %.3f sec\n\n", tott);

  printf("Writing result files and cleaning up.\n");

  /* Count how many candidates we actually have */

  ii = 0;
  while (ii < ncand2 && list[ii].mini_sigma != 0)
    ii++;
  newncand = (ii > cmd->ncand) ? cmd->ncand : ii;

  /* Set our candidate notes to all spaces */

  notes = malloc(sizeof(char) * newncand * 18 + 1);
  for (ii = 0; ii < newncand; ii++)
    strncpy(notes + ii * 18, "                     ", 18);
  
  /* Check the database for possible known PSR detections */
  
  if (idata.ra_h && idata.dec_d) {
    for (ii = 0; ii < newncand; ii++) {
      comp_rawbin_to_cand(&list[ii], &idata, notes + ii * 18, 0);
    }
  }

  /* Compare the candidates with each other */

  compare_rawbin_cands(list, newncand, notes);

  /* Send the candidates to the text file */

  file_rawbin_candidates(list, notes, newncand, cmd->argv[0]);

  /* Write the binary candidate file */

  candfile = chkfopen(candnm, "wb");
  chkfwrite(list, sizeof(rawbincand), (unsigned long) newncand, candfile);
  fclose(candfile);

  /* Free our arrays and close our files */

  if (cmd->stack == 0)
    free(powers);
  free(list);
  free(minifft);
  free(notes);
  fclose(fftfile);
  printf("Done.\n\n");
  return (0);
}
