#ifndef XGBOOST_REGRANK_OBJ_HPP
#define XGBOOST_REGRANK_OBJ_HPP
/*!
 * \file xgboost_regrank_obj.hpp
 * \brief implementation of objective functions
 * \author Tianqi Chen, Kailong Chen
 */
//#include "xgboost_regrank_sample.h"
#include <vector>
#include <functional>
#include "xgboost_regrank_utils.h"

namespace xgboost{
    namespace regrank{        
        class RegressionObj : public IObjFunction{
        public:
            RegressionObj( int loss_type ){
                loss.loss_type = loss_type;
            }
            virtual ~RegressionObj(){}
            virtual void SetParam(const char *name, const char *val){
                if( !strcmp( "loss_type", name ) ) loss.loss_type = atoi( val );
                if( !strcmp( "scale_pos_weight", name ) ) scale_pos_weight = (float)atof( val );
            }
            virtual void GetGradient(const std::vector<float>& preds,  
                                     const DMatrix::Info &info,
                                     int iter,
                                     std::vector<float> &grad, 
                                     std::vector<float> &hess ) {
                utils::Assert( preds.size() == info.labels.size(), "label size predict size not match" );
                grad.resize(preds.size()); hess.resize(preds.size());

                const unsigned ndata = static_cast<unsigned>(preds.size());
                #pragma omp parallel for schedule( static )
                for (unsigned j = 0; j < ndata; ++j){
                    float p = loss.PredTransform(preds[j]);
                    float w = info.GetWeight(j);
                    if( info.labels[j] == 1.0f ) w *= scale_pos_weight;
                    grad[j] = loss.FirstOrderGradient(p, info.labels[j]) * w;
                    hess[j] = loss.SecondOrderGradient(p, info.labels[j]) * w;
                }
            }
            virtual const char* DefaultEvalMetric(void) {
                if( loss.loss_type == LossType::kLogisticClassify ) return "error";
                if( loss.loss_type == LossType::kLogisticRaw ) return "auc";
                return "rmse";
            }
            virtual void PredTransform(std::vector<float> &preds){
                const unsigned ndata = static_cast<unsigned>(preds.size());
                #pragma omp parallel for schedule( static )
                for (unsigned j = 0; j < ndata; ++j){
                    preds[j] = loss.PredTransform( preds[j] );
                }
            }
        private:
            float scale_pos_weight;
            LossType loss;
        };
    };

    namespace regrank{
        // simple softmax rak
        class SoftmaxRankObj : public IObjFunction{
        public:
            SoftmaxRankObj(void){
            }
            virtual ~SoftmaxRankObj(){}
            virtual void SetParam(const char *name, const char *val){
            }
            virtual void GetGradient(const std::vector<float>& preds,  
                                     const DMatrix::Info &info,
                                     int iter,
                                     std::vector<float> &grad, 
                                     std::vector<float> &hess ) {
                utils::Assert( preds.size() == info.labels.size(), "label size predict size not match" );
                grad.resize(preds.size()); hess.resize(preds.size());
                const std::vector<unsigned> &gptr = info.group_ptr;
                utils::Assert( gptr.size() != 0 && gptr.back() == preds.size(), "rank loss must have group file" );
                const unsigned ngroup = static_cast<unsigned>( gptr.size() - 1 );

                #pragma omp parallel
                {
                    std::vector< float > rec;                    
                    #pragma omp for schedule(static)
                    for (unsigned k = 0; k < ngroup; ++k){
                        rec.clear();
                        int nhit = 0;
                        for(unsigned j = gptr[k]; j < gptr[k+1]; ++j ){
                            rec.push_back( preds[j] );
                            grad[j] = hess[j] = 0.0f;
                            nhit += info.labels[j];
                        }
                        Softmax( rec );
                        if( nhit == 1 ){
                            for(unsigned j = gptr[k]; j < gptr[k+1]; ++j ){
                                float p = rec[ j - gptr[k] ];
                                grad[j] = p - info.labels[j];
                                hess[j] = 2.0f * p * ( 1.0f - p );
                            }  
                        }else{
                            utils::Assert( nhit == 0, "softmax does not allow multiple labels" );
                        }
                    }
                }
            }
            virtual const char* DefaultEvalMetric(void) {
                return "pre@1";
            }
        };

        // simple softmax multi-class classification
        class SoftmaxMultiClassObj : public IObjFunction{
        public:
            SoftmaxMultiClassObj(void){
                nclass = 0;
            }
            virtual ~SoftmaxMultiClassObj(){}
            virtual void SetParam(const char *name, const char *val){
                if( !strcmp( "num_class", name ) ) nclass = atoi(val); 
            }
            virtual void GetGradient(const std::vector<float>& preds,  
                                     const DMatrix::Info &info,
                                     int iter,
                                     std::vector<float> &grad, 
                                     std::vector<float> &hess ) {
                utils::Assert( nclass != 0, "must set num_class to use softmax" );
                utils::Assert( preds.size() == (size_t)nclass * info.labels.size(), "SoftmaxMultiClassObj: label size and pred size does not match" );
                grad.resize(preds.size()); hess.resize(preds.size());
                
                const unsigned ndata = static_cast<unsigned>(info.labels.size());
                #pragma omp parallel
                {
                    std::vector<float> rec(nclass);
                    #pragma omp for schedule(static)
                    for (unsigned j = 0; j < ndata; ++j){
                        for( int k = 0; k < nclass; ++ k ){
                            rec[k] = preds[j + k * ndata];
                        }
                        Softmax( rec );
                        int label = static_cast<int>(info.labels[j]);
                        if( label < 0 ){
                            label = -label - 1;
                        }
                        utils::Assert( label < nclass, "SoftmaxMultiClassObj: label exceed num_class" );
                        for( int k = 0; k < nclass; ++ k ){
                            float p = rec[ k ];
                            if( label == k ){
                                grad[j+k*ndata] = p - 1.0f;
                            }else{
                                grad[j+k*ndata] = p;
                            }
                            hess[j+k*ndata] = 2.0f * p * ( 1.0f - p );
                        }  
                    }
                }
            }
            virtual void PredTransform(std::vector<float> &preds){
                utils::Assert( nclass != 0, "must set num_class to use softmax" );
                utils::Assert( preds.size() % nclass == 0, "SoftmaxMultiClassObj: label size and pred size does not match" );                
                const unsigned ndata = static_cast<unsigned>(preds.size()/nclass);
                
                #pragma omp parallel
                {
                    std::vector<float> rec(nclass);
                    #pragma omp for schedule(static)
                    for (unsigned j = 0; j < ndata; ++j){
                        for( int k = 0; k < nclass; ++ k ){
                            rec[k] = preds[j + k * ndata];
                        }
                        preds[j] = FindMaxIndex( rec );
                    }
                }
                preds.resize( ndata );
            }
            virtual const char* DefaultEvalMetric(void) {
                return "merror";
            }
        private:
            int nclass;
        };
    };


    namespace regrank{
        /*! \brief objective for lambda rank */
        class LambdaRankObj : public IObjFunction{
        public:
            LambdaRankObj(void){
                loss.loss_type = LossType::kLogisticRaw;
                fix_list_weight = 0.0f;
                num_pairsample = 1;
            }
            virtual ~LambdaRankObj(){}
            virtual void SetParam(const char *name, const char *val){
                if( !strcmp( "loss_type", name ) )       loss.loss_type = atoi( val );
                if( !strcmp( "fix_list_weight", name ) ) fix_list_weight = (float)atof( val );
                if( !strcmp( "num_pairsample", name ) )  num_pairsample = atoi( val );
            }
        public:
            virtual void GetGradient(const std::vector<float>& preds,  
                                     const DMatrix::Info &info,
                                     int iter,
                                     std::vector<float> &grad, 
                                     std::vector<float> &hess ) {
                utils::Assert( preds.size() == info.labels.size(), "label size predict size not match" );              
                grad.resize(preds.size()); hess.resize(preds.size());
                const std::vector<unsigned> &gptr = info.group_ptr;
                utils::Assert( gptr.size() != 0 && gptr.back() == preds.size(), "rank loss must have group file" );
                const unsigned ngroup = static_cast<unsigned>( gptr.size() - 1 );

                #pragma omp parallel
                {
                    // parall construct, declare random number generator here, so that each 
                    // thread use its own random number generator, seed by thread id and current iteration
                    random::Random rnd; rnd.Seed( iter * 1111 + omp_get_thread_num() );
                    std::vector<LambdaPair> pairs;
                    std::vector<ListEntry>  lst;
                    std::vector< std::pair<float,unsigned> > rec;
                    
                    #pragma omp for schedule(static)
                    for (unsigned k = 0; k < ngroup; ++k){
                        lst.clear(); pairs.clear(); 
                        for(unsigned j = gptr[k]; j < gptr[k+1]; ++j ){
                            lst.push_back( ListEntry(preds[j], info.labels[j], j ) );
                            grad[j] = hess[j] = 0.0f;
                        }                        
                        std::sort( lst.begin(), lst.end(), ListEntry::CmpPred );
                        rec.resize( lst.size() );
                        for( unsigned i = 0; i < lst.size(); ++i ){
                            rec[i] = std::make_pair( lst[i].label, i );
                        }
                        std::sort( rec.begin(), rec.end(), CmpFirst );
                        // enumerate buckets with same label, for each item in the lst, grab another sample randomly
                        for( unsigned i = 0; i < rec.size(); ){
                            unsigned j = i + 1;
                            while( j < rec.size() && rec[j].first == rec[i].first ) ++ j;
                            // bucket in [i,j), get a sample outside bucket
                            unsigned nleft = i, nright = rec.size() - j;
                            if( nleft + nright != 0 ){
                                int nsample = num_pairsample;
                                while( nsample -- ){
                                    for( unsigned pid = i; pid < j; ++ pid ){
                                        unsigned ridx = static_cast<unsigned>( rnd.RandDouble() * (nleft+nright) );
                                        if( ridx < nleft ){
                                            pairs.push_back( LambdaPair( rec[ridx].second, rec[pid].second ) );
                                        }else{
                                            pairs.push_back( LambdaPair( rec[pid].second, rec[ridx+j-i].second ) );
                                        }
                                    }      
                                }
                            }
                            i = j;
                        }
                        // get lambda weight for the pairs
                        this->GetLambdaWeight( lst, pairs );
                        // rescale each gradient and hessian so that the lst have constant weighted
                        float scale = 1.0f / num_pairsample;
                        if( fix_list_weight != 0.0f ){
                            scale *= fix_list_weight / (gptr[k+1] - gptr[k]);
                        }
                        for( size_t i = 0; i < pairs.size(); ++ i ){
                            const ListEntry &pos = lst[ pairs[i].pos_index ];
                            const ListEntry &neg = lst[ pairs[i].neg_index ];
                            const float w = pairs[i].weight * scale;
                            float p = loss.PredTransform( pos.pred - neg.pred );
                            float g = loss.FirstOrderGradient( p, 1.0f );
                            float h = loss.SecondOrderGradient( p, 1.0f );
                            // accumulate gradient and hessian in both pid, and nid, 
                            grad[ pos.rindex ] += g * w; 
                            grad[ neg.rindex ] -= g * w;
                            // take conservative update, scale hessian by 2
                            hess[ pos.rindex ] += 2.0f * h * w; 
                            hess[ neg.rindex ] += 2.0f * h * w;
                        }                       
                    }
                }
            }
            virtual const char* DefaultEvalMetric(void) {
                return "map";
            }
        private:
            // loss function
            LossType loss;
            // number of samples peformed for each instance
            int num_pairsample;            
            // fix weight of each elements in list
            float fix_list_weight;
        protected:
            /*! \brief helper information in a list */
            struct ListEntry{
                /*! \brief the predict score we in the data */
                float pred;
                /*! \brief the actual label of the entry */
                float label;
                /*! \brief row index in the data matrix */                
                unsigned rindex;
                // constructor
                ListEntry(float pred, float label, unsigned rindex): pred(pred),label(label),rindex(rindex){}
                // comparator by prediction
                inline static bool CmpPred(const ListEntry &a, const ListEntry &b){
                    return a.pred > b.pred;
                }
                // comparator by label
                inline static bool CmpLabel(const ListEntry &a, const ListEntry &b){
                    return a.label > b.label;
                }
            };
            /*! \brief a pair in the lambda rank */
            struct LambdaPair{
                /*! \brief positive index: this is a position in the list */
                unsigned pos_index;
                /*! \brief negative index: this is a position in the list */
                unsigned neg_index;
                /*! \brief weight to be filled in */
                float weight;
                LambdaPair( unsigned pos_index, unsigned neg_index ):pos_index(pos_index),neg_index(neg_index),weight(1.0f){}
            };            
            /*! 
             * \brief get lambda weight for existing pairs 
             * \param list a list that is sorted by pred score
             * \param pairs record of pairs, containing the pairs to fill in weights
             */
            virtual void GetLambdaWeight( const std::vector<ListEntry> &sorted_list, std::vector<LambdaPair> &pairs ) = 0;
        };
    };
    
    namespace regrank{
        class PairwiseRankObj: public LambdaRankObj{
        public:
            virtual ~PairwiseRankObj(void){}
            virtual void GetLambdaWeight( const std::vector<ListEntry> &sorted_list, std::vector<LambdaPair> &pairs ){}            
        };


        class LambdaRankObj_NDCG : public LambdaRankObj{
            
        public:
            virtual ~LambdaRankObj_NDCG(void){}

            inline float CalcDCG( const std::vector<float> &labels ){
                double sumdcg = 0.0;
                for( size_t i = 0; i < labels.size(); i ++ ){
                    const unsigned rel = labels[i];
                    if( rel != 0 ){ 
                        sumdcg += logf(2.0f) * ((1<<rel)-1) / logf( i + 2 );
                    }
                }
                return static_cast<float>(sumdcg);
            }

            inline float GetIDCG(const std::vector<ListEntry> &sorted_list){
                std::vector<float> labels;
                for (size_t i = 0; i < sorted_list.size(); i++){
                    labels.push_back(sorted_list[i].label);
                }

                std::sort(labels.begin(), labels.end(), std::greater<float>());
                return CalcDCG(labels);
            }

            /*
            * \brief Obtain the delta NDCG if trying to switch the positions of instances in index1 or index2
            *        in sorted triples. Here DCG is calculated as sigma_i 2^rel_i/log(i + 1)
            * \param sorted_list the list containing entry information
            * \param index1,index2 the instances switched
            * \param the IDCG of the list
            */
            inline float GetLambdaNDCG(const std::vector<ListEntry> &sorted_list,
                int index1,
                int index2, float IDCG){
                double original = (1 << static_cast<int>(sorted_list[index1].label)) / log(index1 + 2)
                    + (1 << static_cast<int>(sorted_list[index2].label)) / log(index2 + 2);
                double changed = (1 << static_cast<int>(sorted_list[index2].label)) / log(index1 + 2)
                    + (1 << static_cast<int>(sorted_list[index1].label)) / log(index2 + 2);
                double ans = (original - changed) / IDCG;
                if (ans < 0) ans = -ans;
                return static_cast<float>(ans);
            }

            virtual void GetLambdaWeight(const std::vector<ListEntry> &sorted_list, std::vector<LambdaPair> &pairs){
                float IDCG = GetIDCG(sorted_list);
                for (size_t i = 0; i < pairs.size(); i++){
                    pairs[i].weight = GetLambdaNDCG(sorted_list,
                        pairs[i].pos_index, pairs[i].neg_index, IDCG);
                }
            }
            
        };

        class LambdaRankObj_MAP : public LambdaRankObj{

            class Quadruple{
            public:
                /* \brief the accumulated precision */
                float ap_acc_;
                /* \brief the accumulated precision assuming a positive instance is missing*/
                float ap_acc_miss_;
                /* \brief the accumulated precision assuming that one more positive instance is inserted ahead*/
                float ap_acc_add_;
                /* \brief the accumulated positive instance count */
                float hits_;

                Quadruple(){}

                Quadruple(const Quadruple& q){
                    ap_acc_ = q.ap_acc_;
                    ap_acc_miss_ = q.ap_acc_miss_;
                    ap_acc_add_ = q.ap_acc_add_;
                    hits_ = q.hits_;
                }

                Quadruple(float ap_acc, float ap_acc_miss, float ap_acc_add, float hits
                    ) :ap_acc_(ap_acc), ap_acc_miss_(ap_acc_miss), ap_acc_add_(ap_acc_add), hits_(hits){

                }

            };

        public:
            virtual ~LambdaRankObj_MAP(void){}

            /*
            * \brief Obtain the delta MAP if trying to switch the positions of instances in index1 or index2
            *        in sorted triples
            * \param sorted_list the list containing entry information
            * \param index1,index2 the instances switched
            * \param map_acc a vector containing the accumulated precisions for each position in a list
            */
            inline float GetLambdaMAP(const std::vector<ListEntry> &sorted_list,
                int index1, int index2,
                std::vector< Quadruple > &map_acc){
                if (index1 == index2 
			|| sorted_list[index1].label == sorted_list[index2].label
			|| map_acc[map_acc.size() - 1].hits_ == 0
			) return 0.0;
                if (index1 > index2) std::swap(index1, index2);
                float original = map_acc[index2].ap_acc_; // The accumulated precision in the interval [index1,index2]
                if (index1 != 0) original -= map_acc[index1 - 1].ap_acc_;
                float changed = 0;
                if (sorted_list[index1].label < sorted_list[index2].label){
                    changed += map_acc[index2 - 1].ap_acc_add_ - map_acc[index1].ap_acc_add_;
                    changed += (map_acc[index1].hits_ + 1.0f) / (index1 + 1);
                }
                else{
                    changed += map_acc[index2 - 1].ap_acc_miss_ - map_acc[index1].ap_acc_miss_;
                    changed += map_acc[index2].hits_ / (index2 + 1);
                }
                if(map_acc[map_acc.size() - 1].hits_ == 0) printf("haha\n");

                float ans = (changed - original) / (map_acc[map_acc.size() - 1].hits_);
                if (ans < 0) ans = -ans;
                return ans;
            }

            /*
            * \brief preprocessing results for calculating delta MAP
            * \return The first field is the accumulated precision, the second field is the
            *         accumulated precision assuming a positive instance is missing,
            *         the third field is the accumulated precision assuming that one more positive
            *         instance is inserted, the fourth field is the accumulated positive instance count
            */
            inline void GetMAPAcc(const std::vector<ListEntry> &sorted_list,
                std::vector< Quadruple > &map_acc){
                map_acc.resize(sorted_list.size());
                float hit = 0, acc1 = 0, acc2 = 0, acc3 = 0;
                for (size_t i = 1; i <= sorted_list.size(); i++){
                    if ((int)sorted_list[i - 1].label > 0) {
                        hit++;
                        acc1 += hit / i;
                        acc2 += (hit - 1) / i;
                        acc3 += (hit + 1) / i;
                    }
                    map_acc[i - 1].ap_acc_ = acc1;
                    map_acc[i - 1].ap_acc_miss_ = acc2;
                    map_acc[i - 1].ap_acc_add_ = acc3;
                    map_acc[i - 1].hits_ = hit;
               
                }
            }
            virtual void GetLambdaWeight(const std::vector<ListEntry> &sorted_list, std::vector<LambdaPair> &pairs){
                std::vector< Quadruple > map_acc;
                GetMAPAcc(sorted_list, map_acc);
                for (size_t i = 0; i < pairs.size(); i++){
                    pairs[i].weight = GetLambdaMAP(sorted_list, pairs[i].pos_index, pairs[i].neg_index, map_acc);
                }
            }
           
        };

    };
};
#endif
