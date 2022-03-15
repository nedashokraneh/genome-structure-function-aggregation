''' This script defines a dataset class including 1D and 2D data types
 and functions to generate or get the data in needed format for different aggregative pipelines'''

import os
import sys
import subprocess
import gzip
sys.path.append("../utilities")
import data_utils
import math
import random
import numpy as np
import pandas as pd
from scipy.sparse import csr_matrix
from collections import Counter
import umap

class dataset:

    '''
    Parameters:
        * cell_type: cell_type corresponding to data
        * resolution: resolution of 1D signals and Hi-C data
        * hic_file_path: path of .hic file including Hi-C data
        * juicer_path: path of a juicer
        * hic_dir_path: path of a directory including dumped hic files and other hic-related files like
         fithic outputs
        * signals_names: names of 1D signals
        * signals_dir_path: path of a directory including binned 1D signals bedgraph files (generated by a script '')
         named '[signal_name].bedgraph'
        * output_dir_path: path of a directory to save models required files
    '''

    def __init__(self, cell_type, assembly, resolution, hic_file_path, juicer_path, hic_dir_path,
    signals_names, signals_dir_path, output_dir_path, data_path, config_path):

        self.cell_type = cell_type
        self.assembly = assembly
        self.resolution = resolution
        self.hic_file_path = hic_file_path
        self.juicer_path = juicer_path
        self.hic_dir_path = hic_dir_path
        if not os.path.exists(self.hic_dir_path):
            os.mkdir(self.hic_dir_path)
        self.signals_names = signals_names
        self.signals_dir_path = signals_dir_path
        self.output_dir_path = output_dir_path
        if not os.path.exists(self.output_dir_path):
            os.mkdir(self.output_dir_path)
        self.output_annots_dir_path = os.path.join(self.output_dir_path, 'annotations')
        if not os.path.exists(self.output_annots_dir_path):
            os.mkdir(self.output_annots_dir_path)
        self.data_path = data_path
        self.config = data_utils.read_config(config_path)
        self.valid_chroms = ['chr{}'.format(c) for c in self.config['valid_chroms']]
        if self.assembly == "hg19":
            self.chr_sizes = data_utils.read_chr_sizes(os.path.join(self.data_path, self.config['hg19_chr_sizes_file']))
            self.chr_arms_sizes = data_utils.read_chr_arm_sizes(os.path.join(self.data_path, self.config['hg19_chr_arms_sizes_file']))
        else:
            self.chr_sizes = data_utils.read_chr_sizes(os.path.join(self.data_path, self.config['hg38_chr_sizes_file']))
        self.labels = {}

    def get_chr_size(self, chrom):

        return({'chr_length': self.chr_sizes['chr{}'.format(chrom)], 'chr_bin_num': math.ceil(self.chr_sizes['chr{}'.format(chrom)]/self.resolution)})


    def load_pos2ind_and_ind2pos_maps(self, valid_bins_file):

        valid_bins_df = pd.read_csv(valid_bins_file, sep = "\t", header = None)
        valid_bins_df.columns = ['chr_name', 'start', 'end', 'index']
        self.valid_bins_df = valid_bins_df.copy()
        self.total_valid_bins = self.valid_bins_df.shape[0]
        valid_bins_df['pos'] = (valid_bins_df['start']/self.resolution).astype(int)
        valid_bins_df['index'] = np.arange(valid_bins_df.shape[0])
        self.valid_bins = {}
        for chrom_name in self.valid_chroms:
            self.valid_bins[chrom_name] = []
        self.pos2ind_dict = {}
        self.ind2pos_dict = []
        for chr_name in self.valid_chroms:
            chrom_size = math.ceil(self.chr_sizes[chr_name]/self.resolution)
            self.pos2ind_dict[chr_name] = np.empty(chrom_size)
            self.pos2ind_dict[chr_name][:] = np.nan
        for i,row in valid_bins_df.iterrows():
            self.valid_bins[row['chr_name']].append(row['pos'])
            self.pos2ind_dict[row['chr_name']][row['pos']] = row['index']
            self.ind2pos_dict.append((row['chr_name'],row['pos']))

    def get_valid_bins(self, chrom):

        return self.valid_bins['chr{}'.format(chrom)]

    def get_chunks_lengths(self):

        start_inds = []
        for chr_name in self.valid_chroms:
            isnan = np.isnan(self.pos2ind_dict[chr_name]).astype(int)
            ls_isnan = np.concatenate((np.array([True]),isnan[:-1]))
            #rs_isnan = np.concatenate((isnan[1:],np.array([True])))
            start_poses = np.where((isnan - ls_isnan) == -1)[0]
            start_inds.extend(list(self.pos2ind_dict[chr_name][start_poses]))
        start_inds = [int(start_ind) for start_ind in start_inds]
        end_inds = start_inds[1:] + [self.total_valid_bins]
        chunks_lengths = [e-s for s,e in zip(start_inds,end_inds)]
        return chunks_lengths

##### Hi-C related functions #####

    def dump_hic_file(self, row_chrom, col_chrom, type):

        file_name = '{}_chr{}_chr{}.txt'.format(type,row_chrom,col_chrom)
        file_path = os.path.join(self.hic_dir_path, file_name)
        if not os.path.exists(file_path):
            print ('dumping {}...'.format(file_name))
            if type == 'oe':
                cmd = ["java", "-jar", self.juicer_path, "dump", type, "KR", self.hic_file_path, str(row_chrom), str(col_chrom), "BP", str(self.resolution), file_path]
            else:
                cmd = ["java", "-jar", self.juicer_path, "dump", type, "None", self.hic_file_path, str(row_chrom), str(col_chrom), "BP", str(self.resolution), file_path]
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            p.wait()

    def get_hic_df(self, row_chrom, col_chrom, type):

        file_name = '{}_chr{}_chr{}.txt'.format(type,row_chrom,col_chrom)
        file_path = os.path.join(self.hic_dir_path, file_name)
        if not os.path.exists(file_path):
            print ('{} does not exist'.format(file_name))
            self.dump_hic_file(row_chrom, col_chrom, type)
        hic_df = pd.read_csv(file_path, sep = "\t", header = None)
        hic_df.columns = ['pos1', 'pos2', 'weight']
        hic_df[['pos1','pos2']] = (hic_df[['pos1','pos2']]/self.resolution).astype(int)
        return hic_df

    def get_indexed_hic_df(self, row_chrom, col_chrom, type):

        file_name = '{}_chr{}_chr{}.txt'.format(type,row_chrom,col_chrom)
        file_path = os.path.join(self.hic_dir_path, file_name)
        if not os.path.exists(file_path):
            print ('{} does not exist'.format(file_name))
            self.dump_hic_file(row_chrom, col_chrom, type)
        hic_df = pd.read_csv(file_path, sep = "\t", header = None)
        hic_df.columns = ['pos1', 'pos2', 'weight']
        hic_df['ind1'] = [self.pos2ind_dict['chr{}'.format(row_chrom)][int(p/self.resolution)] for p in hic_df['pos1']]
        hic_df['ind2'] = [self.pos2ind_dict['chr{}'.format(col_chrom)][int(p/self.resolution)] for p in hic_df['pos2']]
        hic_df = hic_df[['ind1','ind2','weight']]
        hic_df.dropna(inplace = True)
        hic_df['ind1'] = hic_df['ind1'].astype(int)
        hic_df['ind2'] = hic_df['ind2'].astype(int)
        return hic_df



    def get_hic_mat(self, row_chrom, col_chrom, type):

        file_name = '{}_chr{}_chr{}.txt'.format(type,row_chrom,col_chrom)
        file_path = os.path.join(self.hic_dir_path, file_name)
        if not os.path.exists(file_path):
            print ('{} does not exist'.format(file_name))
            self.dump_hic_file(row_chrom, col_chrom, type)
        hic_df = pd.read_csv(file_path, sep = "\t", header = None)
        row_chrom_size = self.get_chr_size(row_chrom)['chr_bin_num']
        col_chrom_size = self.get_chr_size(col_chrom)['chr_bin_num']
        hic_df.columns = ['pos1', 'pos2', 'weight']
        hic_df[['pos1','pos2']] = (hic_df[['pos1','pos2']]/self.resolution).astype(int)
        hic_mat = csr_matrix((hic_df['weight'], (hic_df['pos1'], hic_df['pos2'])), shape=(row_chrom_size,col_chrom_size))
        return hic_mat

    def create_gw_hic_data(self):

        out_file_path = os.path.join(self.output_dir_path, 'GW_HiC.txt')
        #out_file = open(out_file_path, 'w')
        for c1 in np.arange(1,23):
            for c2 in np.arange(c1,23):
                HiC_df = self.get_indexed_hic_df(c1,c2,'oe')
                HiC_df = HiC_df[HiC_df['ind1']!=HiC_df['ind2']]
                HiC_df.to_csv(out_file_path, sep = "\t", header = None, index = False, mode = 'a')
                #for ind,row in HiC_df.iterrows():
                #    line = '{}\t{}\t{}\n'.format(row['ind1'],row['ind2'],row['weight'])
                #    out_file.write(line)
        out_file.close()


    def generate_fithic_file(self, row_chrom, col_chrom):

        hic_file_name = 'observed_chr{}_chr{}.txt'.format(row_chrom,col_chrom)
        hic_file_path = os.path.join(self.hic_dir_path, hic_file_name)
        fithic_output_dir = os.path.join(self.hic_dir_path, 'fithic_outputs')
        curr_fithic_output_dir = os.path.join(fithic_output_dir, 'chr{}_chr{}'.format(row_chrom, col_chrom))
        if os.path.exists(curr_fithic_output_dir):
            print('fithic output for contact map between chromosomes {} and {} was generated before...'.format(row_chrom,col_chrom))
            return
        else:
            if not os.path.exists(hic_file_path):
                print ('{} does not exist...'.format(hic_file_name))
                self.dump_hic_file(row_chrom, col_chrom, 'observed')
            if not os.path.exists(fithic_output_dir):
                os.mkdir(fithic_output_dir)
            data_utils.hic_to_fithic_files(hic_file_path, 'chr{}'.format(row_chrom), 'chr{}'.format(col_chrom), self.resolution, curr_fithic_output_dir)
        os.remove(hic_file_path)

    def get_fithic_output_path(self, row_chrom, col_chrom):

        fithic_output_dir = os.path.join(self.hic_dir_path, 'fithic_outputs', 'chr{}_chr{}'.format(row_chrom, col_chrom))
        fithic_filename = 'FitHiC.spline_pass1.res{}.significances.txt.gz'.format(self.resolution)
        return os.path.join(fithic_output_dir, fithic_filename)

    def get_fithic(self, row_chrom, col_chrom):

        fithic_filepath = self.get_fithic_output_path(row_chrom, col_chrom)
        fithic_df = []
        with gzip.open(fithic_filepath) as f:
            next(f)
            for line in f:
                chr1, fragmentMid1, chr2, fragmentMid2, _, pvalue, *_ = line.decode('UTF-8').split("\t")
                pos1 = math.floor(int(fragmentMid1)/self.resolution)
                pos2 = math.floor(int(fragmentMid2)/self.resolution)
                fithic_df.append({'chr1': chr1, 'pos1': pos1, 'chr2': chr2, 'pos2': pos2, 'pvalue': float(pvalue)})
        fithic_df = pd.DataFrame(fithic_df)
        fithic_df = fithic_df[['chr1','pos1','chr2','pos2','pvalue']]
        return(fithic_df)

    def get_fithic_mat(self, row_chrom, col_chrom, threshold):

        fithic_df = self.get_fithic(row_chrom,col_chrom)
        row_chrom_size = self.get_chr_size(row_chrom)['chr_bin_num']
        col_chrom_size = self.get_chr_size(col_chrom)['chr_bin_num']
        fithic_df['edge'] = fithic_df['pvalue'] < threshold
        fithic_df = fithic_df[fithic_df['edge']]
        fithic_mat = csr_matrix((fithic_df['edge'], (fithic_df['pos1'], fithic_df['pos2'])), shape=(row_chrom_size,col_chrom_size))
        return fithic_mat

    def generate_significant_interactions_file(self, type, th):

        interactions_file_path = os.path.join(self.output_dir_path, '{}_range_interactions.txt'.format(type))
        out_file = open(interactions_file_path, "w")
        if type == 'short':
            for c in self.config['valid_chroms']:
                fithic_filepath = self.get_fithic_output_path(c,c)
                if not os.path.exists(fithic_filepath):
                    self.generate_fithic_file(c,c)
                with gzip.open(fithic_filepath) as f:
                    next(f)
                    for line in f:
                        chr1, fragmentMid1, chr2, fragmentMid2, _, pvalue, *_ = line.decode('UTF-8').split("\t")
                        if (np.isnan(float(pvalue)) or np.isnan(float(fragmentMid1)) or np.isnan(float(fragmentMid2))):
                            continue
                        if (float(pvalue) <= th) & (abs(int(fragmentMid2)-int(fragmentMid1)) <= 1000000):
                            pos1 = math.floor(int(fragmentMid1)/self.resolution)
                            pos2 = math.floor(int(fragmentMid2)/self.resolution)
                            ind1 = self.pos2ind_dict[chr1][pos1]
                            ind2 = self.pos2ind_dict[chr2][pos2]
                            if (np.isnan(ind1) or np.isnan(ind2)):
                                continue
                            if ind1 != ind2:
                                out_file.write("{}\t{}\t{}\n".format(int(ind1),int(ind2),1))
        elif type == 'long':
            for cc,c1 in enumerate(self.config['valid_chroms']):
                for c2 in self.config['valid_chroms'][cc:]:
                    fithic_filepath = self.get_fithic_output_path(c1,c2)
                    if not os.path.exists(fithic_filepath):
                        self.generate_fithic_file(c1,c2)
                    with gzip.open(fithic_filepath) as f:
                        next(f)
                        for line in f:
                            chr1, fragmentMid1, chr2, fragmentMid2, _, pvalue, *_ = line.decode('UTF-8').split("\t")
                            if (np.isnan(float(pvalue)) or np.isnan(float(fragmentMid1)) or np.isnan(float(fragmentMid2))):
                                continue
                            if (float(pvalue) <= th):
                                pos1 = math.floor(int(fragmentMid1)/self.resolution)
                                pos2 = math.floor(int(fragmentMid2)/self.resolution)
                                ind1 = self.pos2ind_dict[chr1][pos1]
                                ind2 = self.pos2ind_dict[chr2][pos2]
                                if (np.isnan(ind1) or np.isnan(ind2)):
                                    continue
                                if ind1 != ind2:
                                    out_file.write("{}\t{}\t{}\n".format(int(ind1),int(ind2),1))

        else:
            print('invalid type. choose between short and long...')
            return

        out_file.close()


    def generate_oe_significant_interactions_file(self, base_count):

        out_file_path = os.path.join(self.output_dir_path, 'oe_significant_interactions.txt')
        chroms_valid_lengths = [len(self.get_valid_bins(c)) for c in np.arange(1,23)]
        min_length = np.min(chroms_valid_lengths)
        for c1 in np.arange(1,23):
            for c2 in np.arange(c1,23):
                c1_valid_length = chroms_valid_lengths[c1-1]
                c2_valid_length = chroms_valid_lengths[c2-1]
                ratio = (c1_valid_length/min_length) * (c2_valid_length/min_length)
                significant_contacts = int(base_count*ratio)
                print('adding {} contacts for chr{} and chr{}'.format(significant_contacts,c1,c2))
                hic_df = self.get_indexed_hic_df(c1,c2,'oe')
                hic_df = hic_df.nlargest(significant_contacts, 'weight')
                hic_df['weight'] = 1
                hic_df = hic_df[hic_df['ind1'] != hic_df['ind2']]
                hic_df.to_csv(out_file_path, sep = "\t", header = None, index = False, mode = 'a')




##### genomic signals related functions #####

    def generate_signals_and_bin_files(self):

        bin_filepath = os.path.join(self.output_dir_path, "bin.txt")
        self.valid_bins_df.to_csv(bin_filepath, sep = "\t", header = None, index = False)
        signals_df = self.valid_bins_df.copy()
        for signal_name in self.signals_names:
            signal_filename = "{}.bedgraph".format(signal_name)
            signal_filepath = os.path.join(self.signals_dir_path, signal_filename)
            signal_df = pd.read_csv(signal_filepath, sep = "\t", header = None)
            signal_df.columns = ['chr_name', 'start', 'end', signal_name]
            signals_df = pd.merge(signals_df, signal_df, on = ['chr_name', 'start', 'end'])
        signals_df = signals_df.loc[:,self.signals_names]
        signals_filepath = os.path.join(self.output_dir_path, "signals.txt")
        signals_df.to_csv(signals_filepath, sep = "\t", header = None, index = False)


    def get_chrom_signals(self, chrom):

        bin_filepath = os.path.join(self.output_dir_path, "bin.txt")
        signals_filepath = os.path.join(self.output_dir_path, "signals.txt")
        bins = pd.read_csv(bin_filepath, sep = "\t", header = None)
        signals = pd.read_csv(signals_filepath, sep = "\t", header = None)
        signals = signals[bins.iloc[:,0] == 'chr{}'.format(chrom)]
        return signals

    def get_signals_and_bins_path(self):

        bin_filepath = os.path.join(self.output_dir_path, "bin.txt")
        signals_filepath = os.path.join(self.output_dir_path, "signals.txt")
        if not os.path.exists(bin_filepath):
            self.generate_signals_and_bin_files()
        return bin_filepath, signals_filepath


##### genomic labels related functions #####

    def align_labels(self, chrom, labels):

        chrom_size = self.get_chr_size(chrom)['chr_bin_num']
        chrom_valid_bins = self.get_valid_bins(chrom)
        aligned_label = np.empty(chrom_size)
        aligned_label[:] = np.nan
        aligned_label[chrom_valid_bins] = labels
        return aligned_label

    def map_genome_wide_annotation(self, labels, label_name):

        self.labels[label_name] = {}
        for chrom in self.config['valid_chroms']:
            chrom_size = self.get_chr_size(chrom)['chr_bin_num']
            self.labels[label_name]['chr{}'.format(chrom)] = np.empty(chrom_size)
            for p in range(chrom_size):
                ind = self.pos2ind_dict['chr{}'.format(chrom)][p]
                if np.isnan(ind):
                    self.labels[label_name]['chr{}'.format(chrom)][p] = np.nan
                else:
                    self.labels[label_name]['chr{}'.format(chrom)][p] = ind

    def write_annotation(self, labels, label_name, out_dir=None):
        if out_dir == None:
            out_dir = self.output_annots_dir_path
        label_out_path = os.path.join(out_dir, '{}_annotation.txt'.format(label_name))
        label_out = open(label_out_path, 'w')
        last_chr, last_pos = self.ind2pos_dict[0]
        last_label = labels[0]
        last_start_pos = last_pos
        for l_ind, label in enumerate(labels[1:]):
            chr_name, pos = self.ind2pos_dict[l_ind+1]
            if (last_chr != chr_name) or (last_pos+1 != pos) or (last_label != label):
                start = last_start_pos * self.resolution
                end = (last_pos+1) * self.resolution
                label_out.write("{}\t{}\t{}\t{}\n".format(last_chr, start, end, last_label))
                last_chr = chr_name
                last_start_pos = pos
                last_label = label
            last_pos = pos
        label_out.close()

    def get_closest_next_ind(self, chr_name, pos):

        next_valid_bins = self.pos2ind_dict[chr_name][pos:]
        next_valid_bins = next_valid_bins[np.isfinite(next_valid_bins)]
        if len(next_valid_bins) > 0:
            closest_next_ind = int(next_valid_bins[0])
            return closest_next_ind
        else:
            return np.nan

    def get_closest_last_ind(self, chr_name, pos):

        last_valid_bins = self.pos2ind_dict[chr_name][:pos+1]
        last_valid_bins = last_valid_bins[np.isfinite(last_valid_bins)]
        if len(last_valid_bins) > 1:
            closest_last_ind = int(last_valid_bins[-1])
            return closest_last_ind
        else:
            return -1


    def read_annotation(self, label_file_path, label_name):

        self.labels[label_name] = np.empty(self.total_valid_bins, dtype=object)
        with open(label_file_path, "r") as f:
            for line in f:
                chr_name, start, end, label, *_ = line.split("\t")
                start_pos = int(int(start)/self.resolution)
                end_pos = int(int(end)/self.resolution)
                start_ind = self.get_closest_next_ind(chr_name,start_pos)
                end_ind = self.get_closest_last_ind(chr_name,end_pos)
                if (np.isnan(start_ind) or np.isnan(end_ind)):
                    continue
                if start_ind <= end_ind:
                    self.labels[label_name][start_ind:end_ind] = label

    def read_segway_annotation(self, annotation_path, label_name):

        self.labels[label_name] = np.empty(self.total_valid_bins, dtype=object)
        with gzip.open(annotation_path, 'rb') as f:
            next(f)
            for line in f:
                chr_name, start, end, label, *_ = line.decode("utf-8").rstrip('\n').split('\t')
                start_ind = self.get_closest_next_ind(chr_name,int(start))
                end_ind = self.get_closest_last_ind(chr_name,int(end))
                if (np.isnan(start_ind) or np.isnan(end_ind)):
                    continue
                if start_ind <= end_ind:
                    self.labels[label_name][start_ind:end_ind] = label

    def get_chr_annotation(self, chrom, label_name):

        if not label_name in self.labels:
            print('{} annotation is not loaded...'.format(label_name))
            return

        chr_size = self.get_chr_size(chrom)['chr_bin_num']
        chr_annot = np.empty(chr_size, dtype=object)
        for p in range(chr_size):
            ind = self.pos2ind_dict['chr{}'.format(chrom)][p]
            if not np.isnan(ind):
                chr_annot[p] = self.labels[label_name][int(ind)]
        return chr_annot


    def read_line_embedding(self, embedding_path, dim):

        embedding_names = ['emb{}'.format(e) for e in np.arange(1,dim+1)]
        embedding_df = pd.read_csv(embedding_path, skiprows = 1, sep = " ", header = None)
        embedding_df = embedding_df.iloc[:,:-1]
        embedding_df.columns = ['index'] + embedding_names
        reducer = umap.UMAP()
        umaps = reducer.fit_transform(embedding_df.loc[:,embedding_names])
        embedding_df['umap1'] = umaps[:,0]
        embedding_df['umap2'] = umaps[:,1]
        embedding_df['chr_name'] = [self.ind2pos_dict[i][0] for i in embedding_df['index']]
        embedding_df['pos'] = [self.ind2pos_dict[i][1] for i in embedding_df['index']]
        #embedding_df = embedding_df[['index','chr_name','pos'] + embedding_names]
        embedding_df = embedding_df[['index','chr_name','pos'] + embedding_names + ['umap1','umap2']]
        embedding_df.sort_values(['index'], inplace=True)
        return embedding_df



    def generate_permutation_map(self):

        r = random.uniform(0,1)
        permutation_map = np.arange(self.total_valid_bins)
        for chr_name in self.valid_chroms:
            base_pos = 0
            for arm in ['p', 'q']:
                arm_size = self.chr_arms_sizes[chr_name,arm]
                arm_size = math.ceil(arm_size/self.resolution)
                poses = np.arange(base_pos,base_pos+arm_size)
                shift_size = math.floor(len(poses)*r)
                shifted_poses = np.roll(poses, shift_size)
                inds = [self.pos2ind_dict[chr_name][p] for p in poses]
                shifted_inds = [self.pos2ind_dict[chr_name][p] for p in shifted_poses]
                inds = np.array(inds)[np.isfinite(inds)].astype(int)
                shifted_inds = np.array(shifted_inds)[np.isfinite(shifted_inds)].astype(int)
                permutation_map[inds] = shifted_inds
                base_pos = arm_size
        return permutation_map