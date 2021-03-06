////////////////////////////////////////////////////////////////////
//
// Main function for implementing (a) Measure Propagation and (b)
// Label Propagation.
//
/////////////////////////////////////////////////////////////////////

#include "MP_large_scale.h"
#include "update_P_and_Q.cc"
#include "reOrderGraph.cc"
#include "miscRoutines.h"
#include <stdint.h>

// create a global version of this, required by miscRoutiens.
struct alternatingMinimizationConfig config;

////////////////////////////////////////////////////////////////////
// to read the labels - the following are done:
//
// (1) read the list of label files into a string vector.
// (2) then open each label file and read in the labels.
// (3) apply appropriate windowing and store the result labels in a 
//     unsigned short vector
// (4) finally reassign all the elements in the label vector to an 
//     array
// (5) if the number of labels < numNodesInGraph, then the label
//     vector is appropriately padded with IGNORE_LABEL
//
unsigned int 
readLabels(node *graph, 
	   const string labelFileList,
	   const string labelFileName, 
	   const unsigned int verbosity,
	   const unsigned int numNodesInGraph,
	   const unsigned short numClasses) {

  vector<string> labelList;
  ifstream iFile;
  if (labelFileList.length() > 0) {
    // read the list of label files. ////////////////////////////////
    printf("Reading list of label files from %s....", labelFileList.c_str());
       
    iFile.open(labelFileList.c_str(), ios::in);
    string tmp;
    
    if ( ! iFile.is_open() )
      GError("unable to open file " + labelFileList, 1);
    
    while (! iFile.eof() ) {
      getline(iFile, tmp, '\n');
      if (tmp.length() > 0)
	labelList.push_back(tmp);
    }
    iFile.close();
  } else { 
    // if all the labels are in a single binary file.
    labelList.push_back(labelFileName);
  }
  /////////////////////////////////////////////////////////////////////////////

  // to store the labels before windowing.
  vector<unsigned short> *tmp_labels = new vector<unsigned short>;
  // to store windowed labels.
  vector<unsigned short> *labels = new vector<unsigned short>;
  // to store measure labels, when using -measureLabels
  vector< vector<float> > *tmp_measure_labels = new vector< vector<float> >;

  int t;
  for (unsigned int i = 0; i < labelList.size(); i++) {
    
    if (verbosity > 10) {
      printf("i = %d and %s\n", i, labelList[i].c_str());
    }

    iFile.open(labelList[i].c_str(), ios::in|ios::binary);
    if (!iFile.is_open()) {
      GError("Unable to open " + labelList[i], 1);
    }

    // if the file was successfully opened
    if (!config.measureLabels) {
        while (true) {
            iFile.read((char*)&t, sizeof(int));
            if (iFile.eof()) break;
            // Sanity check.
            if ( t > numClasses - 1) {
              printf("Found a label %d > numClasses %d in file %s\n",
                     t, numClasses, labelList[i].c_str());
            }
            printf("When tmp_labels.size() == %d, read label: %d\n", (int) tmp_labels->size(), t);
            tmp_labels->push_back(t);
        }
    } else { // use measure labels
        while (true) {
        //for (int j=0; j < numNodesInGraph; j++) {
            float prob[numClasses];
            iFile.read((char*)&prob, sizeof(float)*numClasses);
            //if (iFile.eof()) GError("error reading label files with -measureLabels: not enough values to read", 1);
            if (iFile.eof()) break;
            tmp_measure_labels->resize(tmp_measure_labels->size()+1);
            for (int c = 0; c < numClasses; c++) {
              (*tmp_measure_labels)[tmp_measure_labels->size()-1].push_back(prob[c]);
            }
      }
    }
    iFile.close();
      
    // now window the labels -- need more explanation here
    // What this is doing is truncating the labels at the beginning and at the end.
    // The reason has to do with when doign this for sequential data (such as speech recognition).
    // For example, in speech, you have a sequence and labels of the following form:
    // l1 l2 l3 l4 ...  lN
    // x1 x2 x3 x4 ...  xN
    // where li is the ith label and xi is the ith input vector. Now it is often the case
    // that speech is windowed, i.e., rather than learning a mapping from xi to li directly,
    // you instead learn a mapping from a window around xi to li directly, that is, say from
    //   (x(i-M),x(i-M+1), ..., x(i-1), x(i), x(i+1), ... x(i+M-1), x(i)) to li, and
    // we say that the window size (WS) is 2M+1. Therefore, the first ((WS-1)/2) = M labels
    // are not used. What this little bit of code does is create a label aray without the
    // first M and without the last M labels. Obviously, if WS (whih is the parameter
    // used on the command line) is such that WS=1, then M=0 and nothing is removed.
    for (unsigned int j = (config.nWinSize - 1)/2;
	 j < tmp_labels->size() - (config.nWinSize - 1)/2; j++) {
      labels->push_back( (*tmp_labels)[j] );
    }
    
    // clear for processing next file.
    tmp_labels->clear();
  }
  delete tmp_labels;

  unsigned int numLabels;
  if (config.measureLabels) {
      numLabels = tmp_measure_labels->size();
  } else {
      numLabels = labels->size();
  }
  printf("read %d labels\n", numLabels);
  fflush(stdout);

  // Check to prevent seg-fault.
  if (numLabels > numNodesInGraph) {
    GError("Number of labels is greater than number than number of vertices in graph\n",
	   1);
  }

  // Assign the labels to the graph data structure.
  // If numLabels < numNodesInGraph then the remaining vertices get IGNORE_LABEL.
  // The above is done readGraph.
  node *ptr = graph;
  for (unsigned int i = 0; i < numLabels; ++ptr, ++i) {
    if (!config.measureLabels) {
      ptr->zeroEntropyLabel = (*labels)[i];
    } else {
      ptr->labelDistPtr = new float[numClasses]; // this is freed in freeMemory()
      for (int c = 0; c < numClasses; c++) {
          ptr->labelDistPtr[c] = (*tmp_measure_labels)[i][c];
      }
    }
  }

  delete labels;
  delete tmp_measure_labels;
  return(numLabels);
}


///////////////////////////////////////////////////////////////////
// to read the transduction list
//
// Note that:
// Transduction list setting : +1 => labeled, -1 => unlabeled and
// belongs to dropped-labels set, -2 => unlabeled and belongs to dev
// set 
//
//
unsigned int 
readTransductionList(node *graph, 
		     const string transductionFile,
		     const unsigned int numNodesInGraph,
		     const unsigned int numLabels,
		     const unsigned int verbosity) 
{

  ifstream iFile (transductionFile.c_str(), ios::in|ios::binary);
  if ( ! iFile.is_open() ) {
    GError("unable to open file " + transductionFile, 1);
  }

  // first compute the number of entries in the file. 
  iFile.seekg(0, ios::end);
  unsigned int length = iFile.tellg();
  iFile.seekg(0, ios::beg);
  
  unsigned int lengthTransductionList = length/sizeof(int);  
  if (numNodesInGraph != lengthTransductionList) {
    printf("Length of tranduction list = %d\n", lengthTransductionList);
    printf("Number of vertices in graph = %d\n", numNodesInGraph);
    GError("As you can see, the above number are not equal ", 1);
  }

  int tmp = 0;
  unsigned int counter = 0;
  node *ptr = graph;

  while (!iFile.eof()) {
     iFile.read((char*)&tmp, sizeof(int));
     // initialize whether the vertex is labeled or not based 
     ptr->labeled = 'u'; // a general unlabeled point by default. 
     if (tmp == 1) {
       ptr->labeled = 'l'; // labeled. 
     }
     if (tmp == -2) {
       ptr->labeled = 'd'; // belongs to dev set.
     }
    
     // also initialize the two distributions.
     if (tmp == 1) { // if the node is labeled. 
       float *p = ptr->pDist();
       float *q = ptr->qDist();
       if (!config.measureLabels) {
         for (unsigned int j = 0; j < config.numClasses;
              ++j, ++p, ++q) {
           if ( j == ptr->zeroEntropyLabel ) {
             *p = *q = 1.0;
           } else {
             *p = *q = SMALL;
           }
         }
       } else { // measureLabels == true
         for (unsigned int j = 0; j < config.numClasses;
              ++j, ++p, ++q) {
           if (ptr->labelDistPtr == NULL) GError("Something is wrong with your labels -- you probably gave a bad distribution file\n", 1);
           *p = *q = ptr->labelDistPtr[j];
         }
       }
     }
     ++ptr; ++counter;
     if (counter >= numNodesInGraph) {
       break;
     }
     if (verbosity > 10) {
       fprintf(stderr, "%d ", tmp);
     }
  }
  iFile.close();
  return(lengthTransductionList);
}

/////////////////////////////////////////////////////////////////////
// to read the number of vertices in the graph.
//
//
unsigned int
getNumNodes(const string graphFileName) {
  ifstream iFile;    
  printf("Graph File %s has ", graphFileName.c_str());
  fflush(stdout);
  iFile.open(graphFileName.c_str(), ios::in|ios::binary);
  if (!iFile.is_open()) GError("unable to open " + graphFileName, 1);
  // read the number of vertices.
  unsigned int  nVertices;
  iFile.read((char*)&nVertices, sizeof(unsigned int));
  iFile.close();
  printf("%d vertices\n", nVertices);
  return nVertices;
}


/////////////////////////////////////////////////////////////////////
// to init the nodes in the graph with data from the original graph.
//
//
//
void
readGraph(node *graph, 
	  const string graphFileName, 
	  const unsigned int verbosity, 
	  const unsigned short numClasses,
	  const unsigned int numNodesInGraph,
	  const float sigma) {

  // init random seed
  srand48( time(NULL) );

  ifstream iFile;    
  printf("Reading File %s\t", graphFileName.c_str());
  fflush(stdout);
  iFile.open(graphFileName.c_str(), ios::in|ios::binary);
  if (!iFile.is_open()) GError("unable to open " + graphFileName, 1);

  unsigned int skip;
  iFile.read((char*)&skip, sizeof(unsigned int));
  // allocate memory to accomodate pDist and qDist
  uint64_t len =  2*((uint64_t) numNodesInGraph)*numClasses;
  float *mem_chunk = new float[len];
  assert(mem_chunk != NULL);

  unsigned int ii = 0;
  node *ptr = graph;
  
  //while (!iFile.eof()) {
  while (ii < numNodesInGraph) {
    if (iFile.eof()) {
        printf("Graph file has just %d entries\n", ii-1);
        GError("Graph file too short!\n", 1);
    }

    unsigned int vertex_id = 0, numNeighbors = 0; 

    iFile.read((char*)&vertex_id, sizeof(unsigned int));
    iFile.read((char*)&numNeighbors, sizeof(unsigned short));

    if (verbosity > 50) {
        printf("read vertex: %d ; ii = %d\n", vertex_id, ii);
    }
      
    // if the vertex has no neighbors, then simply got to the next vertex.
    //if (numNeighbors == 0) {
        //continue;
    //}
      
    // if the numNeigbors != 0 and file ends abruptly, get out of the loop.
    //if (iFile.eof()) break;
    if (iFile.eof()) GError("Graph file is messed up!\n", 1);

    // get the next chunk of memory.
    ptr->_pDist = mem_chunk;
    
    // increment pointer so that _pDist points to parameters for next vertex.
    mem_chunk += 2*numClasses;

    float *p = ptr->pDist();
    float *q = ptr->qDist();

    for (unsigned short j = 0; j < numClasses; j++, p++, q++) {
      *p = (drand48() + SMALL)/(1 + SMALL);
      *q = (drand48() + SMALL)/(1 + SMALL);
    }

    // all vertices by default are intialized to having no labels. 
    ptr->labeled = 'n'; 
    if (!config.measureLabels) {
      ptr->zeroEntropyLabel = IGNORE_LABEL;
    } else {
      ptr->labelDistPtr = NULL;
    }

    // populate all neighbor information. 
    ptr->NN = numNeighbors; 
    ptr->idx = new unsigned int[numNeighbors];
    ptr->weight = new float[numNeighbors];
    for (unsigned short i = 0; i < numNeighbors; i++) {
      unsigned int t1;
      float t2;

      iFile.read((char*)&t1, sizeof(unsigned int));
      iFile.read((char*)&t2, sizeof(float));

      ptr->idx[i] = t1;

      // apply the appropriate kernel.
      if (config.applyRBFKernel) {
          ptr->weight[i] = exp( -1 * t2 / sigma); 
          if (ptr->weight[i] <= SMALL) ptr->weight[i] = SMALL;
      }
      ptr->weight[i] = t2;
      // Sanity check.
      if (ptr->weight[i] < 0) {
          printf("Problem with graph, it turns out that weights are negative");
          printf("and this should never happenn");
          GError("", 1);
      }
    }
    ++ii; ++ptr;

  }     
  iFile.close();            
  
  /* debugging
  cout << endl;
  for (int n1=0; n1 < numNodesInGraph; n1++) {
      // for each of n1's neighbors
      cout << n1 << " -- ";
      for (int i=0; i < graph[n1].NN; i++) {
          cout << graph[n1].idx[i] << ":" << graph[n1].weight[i] << ", ";
      }
      cout << endl;
  }
  */

}

/////////////////////////////////////////////////////////////////
// write out parameters in the following format:
// <num_vertices> <num_of_classes>
// <vertex_0> <prob_0>.... <prob_k>
// <vertex_1> ......
//
//
void 
writeOutPosteriors(node *graph, 
		   const unsigned int numNodesInGraph, 
		   const string p_file, 
		   const unsigned short numClasses) {
  
  cout << "Writing out posteriors to " << p_file << endl;

  ofstream oFile;
  oFile.open(p_file.c_str(), ios::out | ios::binary);

  if (! oFile.is_open() )
    GError("Unable to open " + p_file + " for writing", 1);

  // write the number of vertices.
  oFile.write( (char*)&numNodesInGraph, sizeof(unsigned int) );
  // write the number of classes.
  oFile.write( (char*)&numClasses, sizeof(unsigned short) );

  for (unsigned int i = 0; i < numNodesInGraph; i++) {

    // write vertex_number
    oFile.write( (char*)&i, sizeof(unsigned int) ); 
    
    // write all the probs for that vertex.
    const float *p = graph[i].pDist();    

    for (unsigned int j = 0; j < numClasses; j++, p++) 
      oFile.write( (char*)&(*p), sizeof(float) );

  }
  oFile.close();
}


///////////////////////////////////////////////////////////////////////////////////
// to compute the results over all the unlabeled data. note that 
// the results using both p and q are printed out. 
//
//
//
float 
compute_results(node *graph, 
		const unsigned int numNodesInGraph) {

  unsigned int nCorrect_test = 0, nPts_test = 0;
  unsigned int nCorrect_dev = 0, nPts_dev = 0;

  node *ptr = graph;
  for (unsigned int i = 0; i < numNodesInGraph; ++i, ++ptr) {
    
    // if the point is unlabeled.and label is known
    if (ptr->labeled != 'l' && ptr->labeled != 'n') {

      // compute argmax p(z_j)
      float max_label_prob = -1.0;
      int max_label_class = -1;
      float max_post_prob = -1.0;
      int max_post_class = -1;

      float *p = ptr->pDist();

      if (!config.measureLabels) {
        max_label_class = ptr->zeroEntropyLabel;
        max_label_prob = 1;
      }

      // compute the max-probability label and posterior classes
      for (unsigned int j = 0; j < config.numClasses; j++, p++) {
        if (*p > max_post_prob) {
          max_post_prob = *p;
          max_post_class = j;
        }
        if (config.measureLabels) {
          if (ptr->labelDistPtr[j] > max_label_prob) {
            max_label_prob = ptr->labelDistPtr[j];
            max_label_class = j;
          }
        }
      }

      if ((max_post_prob < 0) || (max_label_prob < 0)) {
        printf("Something terrible is happening, your probabilities are");
        printf("perhaps negative\n");
        GError("", 1);
      }

      if ( max_label_class != IGNORE_LABEL ) {
        if (ptr->labeled == 'd') {
          ++nPts_dev;
          nCorrect_dev = ( (unsigned int) max_post_class == max_label_class )?
            (nCorrect_dev + 1):(nCorrect_dev);
        } else {
          ++nPts_test;
          nCorrect_test = ( (unsigned int) max_post_class == max_label_class )?
            (nCorrect_test + 1):(nCorrect_test);
        }
      }
    }
  }

  float perf_test = 100.0 * (float)nCorrect_test/(float)nPts_test;
  float perf_dev = 100.0 * (float)nCorrect_dev/(float)nPts_dev;

  if (nPts_dev > 0) {
    printf("\tNum Dev Samples=%d, Acc=%f\n", nPts_dev, perf_dev);
  }
  if (nPts_test > 0) {
    printf("\tNum Test Samples=%d, Acc=%f\n", nPts_test, perf_test);
  }
  fflush(stdout);

  // if there are no points in the dev set (which is really the test set)
  if ( nPts_dev == 0 )
    return(perf_test);
  
  // if there is a dev set. 
  return(perf_dev);

}

void
objective_value(node *graph,
                const float mu,
                const float nu,
                const float selfWeight,
                const unsigned int numNodesInGraph,
                const unsigned short numClasses,
                float& term1,
                float& term2,
                float& term3,
                float& d_p_q) {

    term1 = 0;
    for (unsigned int i=0; i < numNodesInGraph; i++) {
        node& n = graph[i];
        if (n.labeled == 'l') { 
            if (config.measureLabels) {
                for (int c=0; c < numClasses; c++) {
                    float label_prob = n.labelDistPtr[c];
                    float node_prob = n.qDist()[c];
                    float inc = label_prob * (log(label_prob+SMALL) - log(node_prob+SMALL));
                    term1 += inc;
                }
            } else { // this is untested
                term1 += log(n.qDist()[n.zeroEntropyLabel]);
            }
        }
    }

    // FIXME do selfWeight terms need to be added to this term?
    term2 = 0;
    for (unsigned int i=0; i < numNodesInGraph; i++) {
        node& n = graph[i];
        for (int neigh=0; neigh < n.NN; neigh++) {
            int neigh_index = n.idx[neigh];
            float neigh_weight = n.weight[neigh];
            node& neigh_node = graph[neigh_index];

            for (int c=0; c < numClasses; c++) {
                float node_prob = n.pDist()[c];
                float neigh_prob = neigh_node.qDist()[c];
                float inc = neigh_weight * node_prob * (log(node_prob+SMALL) - log(neigh_prob+SMALL));
                term2 += inc;
            }
        }
    }
    term2 = term2 * mu;

    term3 = 0;
    for (unsigned int i=0; i < numNodesInGraph; i++) {
        node& n = graph[i];
        for (int c=0; c < numClasses; c++) {
            float node_prob = n.pDist()[c];
            float inc = node_prob * log(node_prob+SMALL);
            term3 += inc;
        }
    }
    term3 = term3 * nu;

    d_p_q = 0;
    for (unsigned int i=0; i < numNodesInGraph; i++) {
        node& n = graph[i];

        for (int c=0; c < numClasses; c++) {
            float p_prob = n.pDist()[c];
            float q_prob = n.qDist()[c];
            float inc = p_prob * (log(p_prob+SMALL) - log(q_prob+SMALL));
            d_p_q += inc;
        }
    }
}

/////////////////////////////////////////////////////////////////////
// the main function that does the alternate_minimization
//
//
void 
alternate_minimize(node *graph, 
		   const float mu, 
		   const float nu, 
		   const unsigned int maxIters, 
		   const unsigned int numThreads,
		   const unsigned int numNodesInGraph,
		   const unsigned short numClasses,
		   const bool useSQL,
		   const bool useLP) {

  pthread_t *thread = new pthread_t[numThreads];
  thread_data *data = new thread_data[numThreads];

  pthread_attr_t attr;
  void *status;

  // create an attribute so that the thread can be initialized
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  // to store the objective function value to determine convergence
  float *obj = new float[maxIters]; 
  double *converge = new double[maxIters];
  obj[0] = 0;

  // now do the iterations of alternating minimization. 
  unsigned int iters_until_convergence = maxIters;
  for (unsigned int i = 0; i < maxIters; i++) {

    //printf("\n----------\n");
    printf("Starting Iteration %d...\n", i);
    //printf("----------\n");
    fflush(stdout);

    //////////////////// update P ///////////////////////////////////////////
    //printf("Updating P in.....");
    // first create config.numThreads threads
    for (unsigned int j = 0; j < numThreads; j++) {
      // first populate the data structure that carries arguments to 
      // each of the threads
      data[j].index = j;
      data[j].numThreads = numThreads;
      data[j].numNodesInGraph = numNodesInGraph;
      data[j].numClasses = numClasses;
      data[j].mu = mu;
      data[j].nu = nu;
      data[j].graph = graph;
      data[j].useLP = useLP;
      data[j].conv = 0;

      int rc = 0;
      if (!useSQL) // use kl-divergence based loss
        rc = pthread_create(&thread[j], &attr, update_P,
			    (void *) &data[j]);
      else  // use squared loss
        rc = pthread_create(&thread[j], &attr, update_P_SQL,
			    (void *) &data[j]);
      
      if (rc) 
	GError("Unable to create thread", 1);

    }
    fflush(stdout);
    // free the attribute
    pthread_attr_destroy(&attr);

    // now join threads ... i.e., wait till all of them have finished
    for (unsigned int j = 0; j < config.numThreads; j++) {
      int rc = pthread_join(thread[j], &status); 
      if (rc) {
	GError("Error failed to join thread", 1);
      }      
    }
    
    //////////////////////////////////////////////////////////////////////////

    //////////////////// update Q ///////////////////////////////////////////
    //printf("\nUpdating Q in.....");
    fflush(stdout);
    // first create config.numThreads threads
    for (unsigned int j = 0; j < numThreads; j++) {

      // first populate the data structure that carries arguments to 
      // each of the threads
      data[j].numThreads = numThreads;
      data[j].numNodesInGraph = numNodesInGraph;
      data[j].numClasses = numClasses;
      data[j].index = j;
      data[j].mu = mu;
      data[j].nu = nu;
      data[j].graph = graph;
      data[j].useLP = useLP;
      data[j].conv = 0;
      
      int rc = 0;
      if (!useSQL) // use the Kl-div based loss function
        rc = pthread_create(&thread[j], &attr, update_Q,
			    (void *) &data[j]);
      else // use squared loss
        rc = pthread_create(&thread[j], &attr, update_Q_SQL,
			    (void *) &data[j]);
      
      if (rc) {
	GError("Unable to create thread", 1);
      }
    }

    // free the attribute
    pthread_attr_destroy(&attr);

    // now join threads ... i.e., wait till all of them have finished
    converge[i] = 0;
    for (unsigned int j = 0; j < numThreads; j++) {
      int rc = pthread_join(thread[j], &status); 
      converge[i] += data[j].conv;
      if (rc) {
	GError("Error failed to join thread", 1);      
      }
    }
    //printf("\n");
    fflush(stdout);
    ////////////////////////////////////////////////////////////////////////

    if (config.printAccuracy) {
      compute_results(graph, numNodesInGraph);
    }

    if (i > 0) {
      float change = (converge[i-1] - converge[i])/converge[i];
      //printf("Objective value at iter %d: %f\n", i, converge[i]);
      //printf("Change in convergence criteria = %f\n", change);
      if ( change <= CONVERGENCE_CRITERIA ) {
          printf("Convergence criterion reached on iteration %d\n", i);
          iters_until_convergence = i;
          i = maxIters + 1u;
      } 
      printf("Relative objective value: %e\t Change:%e\n", converge[i], change);
    }

  }

    // Write out the parameters ONLY if a output posterior file has 
    // been specified. 
    if (config.outPosteriorFile.length() > 0) {
      char buffer[256];
      if (config.readWeightsFile) {
          // If we picked multiple mu/nu's from a file,
          // put mu and nu in filename
          printf("Writing mu and nu to posterior filename\n");
          sprintf(buffer, "%s_mu=%f_nu=%f", config.outPosteriorFile.c_str(), mu, nu);
      } else {
          // If we read mu and nu from the command line, don't put them
          // in the filenames
          printf("NOT Writing mu and nu to posterior filename\n");
          sprintf(buffer, config.outPosteriorFile.c_str());
      }
      string outfilename = buffer;
      writeOutPosteriors(graph, numNodesInGraph, 
              outfilename,
              config.numClasses);
    }

    if (config.outObjFile.length() > 0) {
        float term1;
        float term2;
        float term3;
        float d_p_q;
        objective_value(graph, mu, nu, config.selfWeight, numNodesInGraph, numClasses, term1, term2, term3, d_p_q);

        ofstream objfile(config.outObjFile.c_str());
        objfile << "total\t"<< term1 + term2 + term3 << endl;
        objfile << "term1\t"<< term1 << endl;
        objfile << "term2\t"<< term2 << endl;
        objfile << "term3\t"<< term3 << endl;
        objfile << "d_p_q\t"<< d_p_q << endl;
        objfile << "iters_until_convergence\t"<< iters_until_convergence << endl;
        objfile.close();
    }

  //printf("mu = %f, nu = %f, acc = %f\n", mu, nu, tmp);
  fflush(stdout);

  delete[] thread;
  delete[] data;
  delete[] obj;  
}


///////////////////////////////////////////////////////////
// get statistics about the graph -- this is really for 
// debugging..
//
//
void 
getStats(node *graph, unsigned int numNodesInGraph) {

  unsigned int numLabeled = 0;
  unsigned int numUnLabeled = 0;
  unsigned int numDevSet = 0;
  unsigned int numNodes = 0;

  for (unsigned int i = 0; i < numNodesInGraph; i++) {
    node *tmp = &graph[i];

    if (tmp->labeled == 'l') 
      numLabeled++;
    else if (tmp->labeled == 'd')
      numDevSet++;
    else 
      numUnLabeled++;
    
    numNodes++;

  }

  cout << "Number of Nodes = " << numNodes << endl;
  cout << "Number of Labeled = " << numLabeled << endl;
  cout << "Number of Unlabeled = " << numUnLabeled << endl;
  cout << "Number of Dev Set = " << numDevSet << endl;

}

/////////////////////////////////////////////////////////////////
// a debug routine that given idx, returns information
// about that node in the graph. 
// Information includes, number of NN's, idx & weights of NN's
//
void 
lookAtNode(const node *graph, unsigned int idx) {
  
  cout << "Node #" << idx << endl;
  cout << "Positions -- " << graph[idx].index  << endl;
  for (unsigned int i = 0; i < graph[idx].NN; i++) {
    cout << "NN_idx = " << graph[idx].idx[i] << " NN_weight = " 
	 << graph[idx].weight[i] << endl;
  }

}




//////////////////////////////////////////////////////////////////////////
// to re-init all the distributions in the graph
//
//
//
void 
reInit(node *graph, 
       const unsigned int numNodesInGraph,
       const unsigned short numClasses) {
  
  //fprintf(stderr, "Reinitializing distributions...\n");
  fflush(stderr);

  srand48( time(NULL) );
  float r;
  node *ptr = graph;
  
  for (unsigned int i = 0; i < numNodesInGraph; ++i, ++ptr) {
    
     float *p = ptr->pDist();
     float *q = ptr->qDist();
     
     // if vertex is labeled.
     if (ptr->labeled == 'l') {     
       for (unsigned int j = 0; j < numClasses; j++, p++, q++) {
         if (!config.measureLabels) {
           if ( j == graph[i].zeroEntropyLabel )
             *p = *q = 1.0;
           else
             *p = *q = SMALL;
         } else {
           *p = *q = graph[i].labelDistPtr[j];
         }
       }       
     } else { // if vertex is unlabeled
       float sump = 0.0, sumq = 0.0;
       for (unsigned int j = 0; j < numClasses; j++, p++, q++) {
	 r = (drand48() + SMALL)/(1 + SMALL);
	 *p = r; sump += r;
	 r = (drand48() + SMALL)/(1 + SMALL);
	 *q = r; sumq += r;
       }
       // ensure p and q sum to 1;
       float *p = ptr->pDist();
       float *q = ptr->qDist();
       for (unsigned int j = 0; j < numClasses; ++j, ++p, ++q) {
	 *p /= sump;
	 *q /= sumq;
       }
     }
     
     if ( !(i % 1000000) )  {
       fprintf(stderr,":");
       fflush(stderr);
     }
  }
  fprintf(stderr,"\n");
  fflush(stderr);
}


/////////////////////////////////////////////////////////////////////
// to read the file containing the mu's and nu's
// it is the assume that the file is written at
// <num_mu_entries> \n mu_1\n mu_2....
// <num_nu_entries> \n nu_1\n nu_2\n....
//
//
void 
readWeightsFile(const string weightsFile,
		float *mu, float *nu, 
		unsigned int *lenMu, 
		unsigned int *lenNu) {

  ifstream iFile (weightsFile.c_str(), ios::in);
  string tmp;

  printf("**********************************************\n");

  if (iFile.is_open()) {
    // read the number of mu's
    getline(iFile, tmp, '\n');
    istringstream buffer(tmp);
    unsigned int t;
    buffer >> t;
    
    // now assign the required storage. 
    *lenMu = t;
    
    // now read the actual mu's 
    for (unsigned int i = 0; i < t; i++) {
        getline(iFile, tmp, '\n');
	istringstream buffer(tmp);
	float tf;
	buffer >> tf;
	mu[i] = tf;           
    }

    printf("Read the following values for mu: ");
    for (unsigned int i = 0; i < t; i++) 
      printf("%f ", mu[i]);
    printf("\n");
    fflush(stdout);

    // read the number of mu's
    getline(iFile, tmp, '\n');
    istringstream buffer1(tmp);
    buffer1 >> t;

    // now assign the required storage. 
    *lenNu = t;
	   

    // now read the actual mu's 
    for (unsigned int i = 0; i < t; i++) {
      getline(iFile, tmp, '\n');
      istringstream buffer(tmp);
      float tf;
      buffer >> tf;
      nu[i] = tf;           
    }

    printf("Read the following values for nu: ");
    for (unsigned int i = 0; i < t; i++) 
      printf("%f ", nu[i]);
    printf("\n\n");
    fflush(stdout);
    
	
  }

  printf("**********************************************\n");
  fflush(stdout);

}



////////////////////////////////////////////////////////////////////////
// to free memory associated with the graph
//
//
void
freeMemory(node *graph, 
	   unsigned int numNodesInGraph) {

  node *tmp = graph;
  delete[] tmp->_pDist;

  for (unsigned int i = 0; i < numNodesInGraph; i++, tmp++)  {
    
    delete[] tmp->weight;
    delete[] tmp->idx;
    if (config.measureLabels) {
      delete[] tmp->labelDistPtr;
    }
    
  }

}


////////////////////////////////////////////////////////////////////////
// and the most imp. function of them all.
//
//
//
int 
main(int argc, char *argv[]) {


  struct rusage rus; /* starting time */
  struct rusage rue; /* ending time */

  // get start time.
  getrusage(RUSAGE_SELF,&rus);

  // set the defaults
  config.setDefaults();

  // process the command line arguments
  processCmdArgs(config,argc,argv);

  ////// set thing up for fast log /////////////////////////////////////

#if USE_FAST_LOG

  printf("***********************\n");
  printf("Using FAST log()\n");
  printf("***********************\n");

  int tabsize = (int) pow(2,TABEXP);
  mytable = (float*) malloc(tabsize*sizeof(float));

  // compute the look-up table.
  do_table(mytable,TABEXP);

#else
  
  //printf("***********************\n");
  //printf("Using math.h's log()\n");
  //printf("***********************\n");

#endif

  /////////////////////////////////////////////////////////////////////

  // read the weights file (mu's and nu's) ///////////////////////////
  float *mu = new float[MAX_MU_NU_LEN]; 
  float *nu = new float[MAX_MU_NU_LEN]; 
  unsigned int lenMu, lenNu;
  if (!config.useLP) { // no LP
    if (config.readWeightsFile) {// read a file containing mu and nu
      readWeightsFile(config.weightsFile,
		      mu, 
		      nu, 
		      &lenMu, 
		      &lenNu);
    } else { // no weights file, but mu and nu specified on command line.
      mu[0] = config.mu;
      nu[0] = config.nu;
      lenMu = lenNu = 1;
    }
  } else { // use label propagation
    lenMu = lenNu = 1;
    mu[0] = 1.0;
    nu[0] = 0.0;
  }
  
  // read the graph ////////////////////////////////////////////////////
  printf("\n*********************************************************\n");
  unsigned int numNodesInGraph = getNumNodes(config.inputGraphName);
  node *graph = new node[numNodesInGraph];
  readGraph(graph, 
	    config.inputGraphName, 
	    config.verbosity, 
	    config.numClasses,
	    numNodesInGraph,
	    config.sigma);
  printf("Read graph with %d vertices\n", numNodesInGraph);
  printf("*********************************************************\n");
  //////////////////////////////////////////////////////////////////////
  
  // read the label file into memory. ////////////////////////////////////
  printf("\n*********************************************************\n");
  unsigned int numLabels = readLabels(graph, 
				      config.labelFileList,
				      config.labelFile, 
				      config.verbosity, 
				      numNodesInGraph,
				      config.numClasses);  
  //////////////////////////////////////////////////////////////////////////

  // read the transduction file into memory ////////////////////////////////
  unsigned int lengthTransductionList = readTransductionList(graph, 
					       config.inputTransductionFile,
					       numNodesInGraph,
					       numLabels,
					       config.verbosity);
  printf("Read Transduction list with %d entries from %s\n", 
	 lengthTransductionList, (config.inputTransductionFile).c_str());
  fflush(stdout);
  printf("*********************************************************\n");
  //////////////////////////////////////////////////////////////////////////
  
  // sort the graph ///////////////////////////////////////////////////////

  if ( config.reOrderGraph ) {

    /*    lookAtNode(graph, 0);
	  lookAtNode(graph, 1);*/

    printf("Average Cardinality of Intersetion Before Re-ordering = %f\n",
	   computeAverageIntersectionCard(graph, numNodesInGraph));

    graph = reOrderGraph(graph, numNodesInGraph);

    printf("Average Cardinality of Intersetion After Re-ordering = %f\n",
	   computeAverageIntersectionCard(graph, numNodesInGraph));

    /*    lookAtNode(graph, 0);
	  lookAtNode(graph, 1);*/
  }

  // done with all init.. get total time for init. 
  getrusage(RUSAGE_SELF,&rue);

  // print out time spent so far.
  printf("\n*****************************************************************\n");
  printf("Total time for reading graph + setting up data structures ");
  reportTiming(rus,rue);
  printf("*****************************************************************\n");
  fflush(stdout);

  ////////////////////////////////////////////////////////////////////

  // do alternating minimization /////////////////////////////////////////////
  printf("\n******************************\n");
  getStats(graph, numNodesInGraph);
  printf("*******************************\n");

  printf("\n*************************************\n");
  printf("Starting Alternating Minimization....\n");
  fflush(stdout);

  for (unsigned int i = 0; i < lenMu; i++) {
    for (unsigned int j = 0; j < lenNu; j++) {

      printf("Starting iteration for mu = %f, nu = %f\n", mu[i], nu[j]);
      fflush(stdout);

      alternate_minimize(graph, mu[i], nu[j],
			 config.maxIters, 
			 config.numThreads, 
			 numNodesInGraph, 
			 config.numClasses,
			 config.useSQL,
			 config.useLP);

      printf("End of iteration for mu = %f, nu = %f\n", mu[i], nu[j]);
      printf("\n*************************************\n");
      fflush(stdout);
      
      if (  !( ( i == (lenMu - 1) ) && ( j == (lenNu - 1) ) ) ) {
	// to re-initialize the distributions in the graph so that 
	// alternate_minimize can be run another time. 
	reInit(graph,
	       numNodesInGraph,
	       config.numClasses);
      }
    }
  }

  ////////////////////////////////////////////////////////////////////////////

  printf("Done Training using %d threads\n", config.numThreads);
  fflush(stdout);

  ///////////////////////////////////////////////////////////////////////////

  // free up all the memory that has been uses /////////////////////////////

  freeMemory(graph, 
	     numNodesInGraph);

  delete[] graph;
  delete[] mu;
  delete[] nu;

#if USE_FAST_LOG
  delete[] mytable;
#endif

  ///////////////////////////////////////////////////////////////////////////

  return(0);

}

