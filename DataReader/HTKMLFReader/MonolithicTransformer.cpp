#include "stdafx.h"
#include "MonolithicTransformer.h"

#ifdef _WIN32
#include <objbase.h>
#endif
#include "Basics.h"

#include "htkfeatio.h"                  // for reading HTK features
#include "latticearchive.h"             // for reading HTK phoneme lattices (MMI training)
#include "msra_mgram.h"                 // for unigram scores of ground-truth path in sequence training

#include "rollingwindowsource.h"        // minibatch sources
#include "utterancesourcemultiNew.h"
#include "chunkevalsource.h"
#include "minibatchiterator.h"
#define DATAREADER_EXPORTS  // creating the exports here
#include "DataReader.h"
#include "commandArgUtil.h"
#include "ScriptableObjects.h"
#include "HTKMLFReader.h"
#include "TimerUtility.h"

#ifdef __unix__
#include <limits.h>
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef unsigned int UNINT32;
#endif
#pragma warning (disable: 4127) // conditional expression is constant; "if (sizeof(ElemType)==sizeof(float))" triggers this

#include "TimerUtility.h"
#include "Utils.h"
#include "Bundler.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    MonolithicTransformer::MonolithicTransformer(const ConfigParameters & readerConfig, size_t elementSize)
        : m_elementSize(elementSize)
    {
        intargvector numberOfuttsPerMinibatchForAllEpochs =
            readerConfig(L"nbruttsineachrecurrentiter", ConfigParameters::Array(intargvector(vector<int>{ 1 })));

        m_numSeqsPerMBForAllEpochs = numberOfuttsPerMinibatchForAllEpochs;

        for (int i = 0; i < m_numSeqsPerMBForAllEpochs.size(); i++)
        {
            if (m_numSeqsPerMBForAllEpochs[i] < 1)
            {
                LogicError("nbrUttsInEachRecurrentIter cannot be less than 1.");
            }
        }

        m_numSeqsPerMB = m_numSeqsPerMBForAllEpochs[0];

        m_noData = false;

        wstring command(readerConfig(L"action", L"")); //look up in the config for the master command to determine whether we're writing output (inputs only) or training/evaluating (inputs and outputs)

        if (readerConfig.Exists(L"legacyMode"))
            RuntimeError("legacy mode has been deprecated\n");

        vector<wstring> scriptpaths;
        vector<wstring> RootPathInScripts;
        vector<wstring> mlfpaths;
        vector<vector<wstring>>mlfpathsmulti;
        size_t firstfilesonly = SIZE_MAX;   // set to a lower value for testing
        vector<vector<wstring>> infilesmulti;
        size_t numFiles;
        wstring unigrampath(L"");
        size_t randomize = randomizeAuto;
        size_t iFeat, iLabel;
        iFeat = iLabel = 0;
        vector<wstring> statelistpaths;
        vector<size_t> numContextLeft;
        vector<size_t> numContextRight;

        std::vector<std::wstring> featureNames;
        std::vector<std::wstring> labelNames;
        // for hmm and lattice 
        std::vector<std::wstring> hmmNames;
        std::vector<std::wstring> latticeNames;
        Utils::GetDataNamesFromConfig(readerConfig, featureNames, labelNames, hmmNames, latticeNames);
        if (featureNames.size() + labelNames.size() <= 1)
        {
            InvalidArgument("network needs at least 1 input and 1 output specified!");
        }
        size_t input_index = 0;
        //load data for all real-valued inputs (features)
        foreach_index(i, featureNames)
        {
            const ConfigParameters & thisFeature = readerConfig(featureNames[i]);
            m_featDims.push_back(thisFeature(L"dim"));
            intargvector contextWindow = thisFeature(L"contextWindow", ConfigParameters::Array(intargvector(vector<int>{ 1 })));
            if (contextWindow.size() == 1) // symmetric
            {
                size_t windowFrames = contextWindow[0];
                if (windowFrames % 2 == 0)
                    InvalidArgument("augmentationextent: neighbor expansion of input features to %d not symmetrical", (int)windowFrames);
                size_t context = windowFrames / 2;           // extend each side by this
                numContextLeft.push_back(context);
                numContextRight.push_back(context);

            }
            else if (contextWindow.size() == 2) // left context, right context
            {
                numContextLeft.push_back(contextWindow[0]);
                numContextRight.push_back(contextWindow[1]);
            }
            else
            {
                InvalidArgument("contextFrames must have 1 or 2 values specified, found %d", (int)contextWindow.size());
            }
            // update m_featDims to reflect the total input dimension (featDim x contextWindow), not the native feature dimension
            // that is what the lower level feature readers expect
            m_featDims[i] = m_featDims[i] * (1 + numContextLeft[i] + numContextRight[i]);

            wstring type = thisFeature(L"type", L"real");
            if (!_wcsicmp(type.c_str(), L"real"))
            {
                m_nameToTypeMap[featureNames[i]] = InputOutputTypes::real;
            }
            else
            {
                InvalidArgument("feature type must be 'real'");
            }

            FrameDescription featureFrameDescription;
            featureFrameDescription.elementSize = m_elementSize;
            featureFrameDescription.dimensions.push_back(m_featDims[i]);
            m_featureFrameDescriptions.push_back(featureFrameDescription);


            m_featureNameToIdMap[featureNames[i]] = iFeat;
            scriptpaths.push_back(thisFeature(L"scpFile"));
            RootPathInScripts.push_back(thisFeature(L"prefixPathInSCP", L""));
            m_featureNameToDimMap[featureNames[i]] = m_featDims[i];

            m_nameToId.insert(std::make_pair(featureNames[i], input_index));

            InputDescriptionPtr input = std::make_shared<InputDescription>();
            input->name = featureNames[i];
            input->id = input_index;
            input->sampleLayout = std::make_shared<ImageLayout>(std::vector<size_t> { m_featDims[i] });
            m_inputs.push_back(input);

            input_index++;
            iFeat++;
        }

        foreach_index(i, labelNames)
        {
            const ConfigParameters & thisLabel = readerConfig(labelNames[i]);
            if (thisLabel.Exists(L"labelDim"))
                m_labelDims.push_back(thisLabel(L"labelDim"));
            else if (thisLabel.Exists(L"dim"))
                m_labelDims.push_back(thisLabel(L"dim"));
            else
                InvalidArgument("labels must specify dim or labelDim");

            wstring type;
            if (thisLabel.Exists(L"labelType"))
                type = (const wstring &)thisLabel(L"labelType"); // let's deprecate this eventually and just use "type"...
            else
                type = (const wstring &)thisLabel(L"type", L"category"); // outputs should default to category

            if (!_wcsicmp(type.c_str(), L"category"))
                m_nameToTypeMap[labelNames[i]] = InputOutputTypes::category;
            else
                InvalidArgument("label type must be 'category'");

            statelistpaths.push_back(thisLabel(L"labelMappingFile", L""));

            m_labelNameToIdMap[labelNames[i]] = iLabel;
            m_labelNameToDimMap[labelNames[i]] = m_labelDims[i];
            mlfpaths.clear();
            if (thisLabel.ExistsCurrent(L"mlfFile"))
            {
                mlfpaths.push_back(thisLabel(L"mlfFile"));
            }
            else
            {
                if (!thisLabel.ExistsCurrent(L"mlfFileList"))
                {
                    InvalidArgument("Either mlfFile or mlfFileList must exist in HTKMLFReder");
                }
                wstring list = thisLabel(L"mlfFileList");
                for (msra::files::textreader r(list); r;)
                {
                    mlfpaths.push_back(r.wgetline());
                }
            }
            mlfpathsmulti.push_back(mlfpaths);

            FrameDescription labelFrameDescription;
            labelFrameDescription.elementSize = m_elementSize;
            labelFrameDescription.dimensions.push_back(m_labelDims[i]);
            m_labelFrameDescriptions.push_back(labelFrameDescription);

            m_nameToId.insert(std::make_pair(labelNames[i], input_index));

            InputDescriptionPtr input = std::make_shared<InputDescription>();
            input->name = labelNames[i];
            input->id = input_index;
            input->sampleLayout = std::make_shared<ImageLayout>(std::vector<size_t> { m_labelDims[i] });
            m_inputs.push_back(input);

            input_index++;
            iLabel++;
        }

        //get lattice toc file names 
        std::pair<std::vector<wstring>, std::vector<wstring>> latticetocs;
        foreach_index(i, latticeNames)  //only support one set of lattice now
        {
            const ConfigParameters & thisLattice = readerConfig(latticeNames[i]);

            vector<wstring> paths;
            expand_wildcards(thisLattice(L"denLatTocFile"), paths);
            latticetocs.second.insert(latticetocs.second.end(), paths.begin(), paths.end());

            if (thisLattice.Exists(L"numLatTocFile"))
            {
                paths.clear();
                expand_wildcards(thisLattice(L"numLatTocFile"), paths);
                latticetocs.first.insert(latticetocs.first.end(), paths.begin(), paths.end());
            }

        }

        //get HMM related file names
        vector<wstring> cdphonetyingpaths, transPspaths;
        foreach_index(i, hmmNames)
        {
            const ConfigParameters & thisHMM = readerConfig(hmmNames[i]);

            cdphonetyingpaths.push_back(thisHMM(L"phoneFile"));
            transPspaths.push_back(thisHMM(L"transPFile", L""));
        }

        if (iFeat != scriptpaths.size() || iLabel != mlfpathsmulti.size())
            RuntimeError("# of inputs files vs. # of inputs or # of output files vs # of outputs inconsistent\n");

        if (readerConfig.Exists(L"randomize"))
        {
            wstring randomizeString = readerConfig.CanBeString(L"randomize") ? readerConfig(L"randomize") : wstring();
            if (!_wcsicmp(randomizeString.c_str(), L"none"))
                randomize = randomizeNone;
            else if (!_wcsicmp(randomizeString.c_str(), L"auto"))
                randomize = randomizeAuto;
            else
                randomize = readerConfig(L"randomize");
        }

        m_verbosity = readerConfig(L"verbosity", 2);

        // determine if we partial minibatches are desired
        wstring minibatchMode(readerConfig(L"minibatchMode", L"partial"));
        m_partialMinibatch = !_wcsicmp(minibatchMode.c_str(), L"partial");

        // get the read method, defaults to "blockRandomize" other option is "rollingWindow"
        wstring readMethod(readerConfig(L"readMethod", L"blockRandomize"));

        if (readMethod == L"blockRandomize" && randomize == randomizeNone)
            InvalidArgument("'randomize' cannot be 'none' when 'readMethod' is 'blockRandomize'.");

        // read all input files (from multiple inputs)
        // TO DO: check for consistency (same number of files in each script file)
        numFiles = 0;
        foreach_index(i, scriptpaths)
        {
            vector<wstring> filelist;
            std::wstring scriptpath = scriptpaths[i];
            fprintf(stderr, "reading script file %ls ...", scriptpath.c_str());
            size_t n = 0;
            for (msra::files::textreader reader(scriptpath); reader && filelist.size() <= firstfilesonly/*optimization*/;)
            {
                filelist.push_back(reader.wgetline());
                n++;
            }

            fprintf(stderr, " %lu entries\n", n);

            if (i == 0)
                numFiles = n;
            else
                if (n != numFiles)
                    RuntimeError("number of files in each scriptfile inconsistent (%d vs. %d)", (int)numFiles, (int)n);

            // post processing file list : 
            //  - if users specified PrefixPath, add the prefix to each of path in filelist
            //  - else do the dotdotdot expansion if necessary 
            wstring rootpath = RootPathInScripts[i];
            if (!rootpath.empty()) // use has specified a path prefix for this  feature 
            {
                // first make slash consistent (sorry for linux users:this is not necessary for you)
                std::replace(rootpath.begin(), rootpath.end(), L'\\', L'/');
                // second, remove trailling slash if there is any 
                std::wregex trailer(L"/+$");
                rootpath = std::regex_replace(rootpath, trailer, wstring());
                // third, join the rootpath with each entry in filelist 
                if (!rootpath.empty())
                {
                    for (wstring & path : filelist)
                    {
                        if (path.find_first_of(L'=') != wstring::npos)
                        {
                            vector<wstring> strarr = msra::strfun::split(path, L"=");
#ifdef WIN32
                            replace(strarr[1].begin(), strarr[1].end(), L'\\', L'/');
#endif 

                            path = strarr[0] + L"=" + rootpath + L"/" + strarr[1];
                        }
                        else
                        {
#ifdef WIN32
                            replace(path.begin(), path.end(), L'\\', L'/');
#endif 
                            path = rootpath + L"/" + path;
                        }
                    }
                }
            }
            else
            {
                /*
                do "..." expansion if SCP uses relative path names
                "..." in the SCP means full path is the same as the SCP file
                for example, if scp file is "//aaa/bbb/ccc/ddd.scp"
                and contains entry like
                .../file1.feat
                .../file2.feat
                etc.
                the features will be read from
                //aaa/bbb/ccc/file1.feat
                //aaa/bbb/ccc/file2.feat
                etc.
                This works well if you store the scp file with the features but
                do not want different scp files everytime you move or create new features
                */
                wstring scpdircached;
                for (auto & entry : filelist)
                    Utils::ExpandDotDotDot(entry, scriptpath, scpdircached);
            }

            infilesmulti.push_back(std::move(filelist));
        }

        if (readerConfig.Exists(L"unigram"))
            unigrampath = (const wstring &)readerConfig(L"unigram");

        // load a unigram if needed (this is used for MMI training)
        msra::lm::CSymbolSet unigramsymbols;
        std::unique_ptr<msra::lm::CMGramLM> unigram;
        size_t silencewordid = SIZE_MAX;
        size_t startwordid = SIZE_MAX;
        size_t endwordid = SIZE_MAX;
        if (unigrampath != L"")
        {
            unigram.reset(new msra::lm::CMGramLM());
            unigram->read(unigrampath, unigramsymbols, false/*filterVocabulary--false will build the symbol map*/, 1/*maxM--unigram only*/);
            silencewordid = unigramsymbols["!silence"];     // give this an id (even if not in the LM vocabulary)
            startwordid = unigramsymbols["<s>"];
            endwordid = unigramsymbols["</s>"];
        }

        if (!unigram && latticetocs.second.size() > 0)
            fprintf(stderr, "trainlayer: OOV-exclusion code enabled, but no unigram specified to derive the word set from, so you won't get OOV exclusion\n");

        // currently assumes all mlfs will have same root name (key)
        set<wstring> restrictmlftokeys;     // restrict MLF reader to these files--will make stuff much faster without having to use shortened input files
        if (infilesmulti[0].size() <= 100)
        {
            foreach_index(i, infilesmulti[0])
            {
                msra::asr::htkfeatreader::parsedpath ppath(infilesmulti[0][i]);
                const wstring key = regex_replace((wstring)ppath, wregex(L"\\.[^\\.\\\\/:]*$"), wstring());  // delete extension (or not if none)
                restrictmlftokeys.insert(key);
            }
        }
        // get labels

        double htktimetoframe = 100000.0;           // default is 10ms 
        std::vector<std::map<std::wstring, std::vector<msra::asr::htkmlfentry>>> labelsmulti;
        foreach_index(i, mlfpathsmulti)
        {
            const msra::lm::CSymbolSet* wordmap = unigram ? &unigramsymbols : NULL;
            msra::asr::htkmlfreader<msra::asr::htkmlfentry, msra::lattices::lattice::htkmlfwordsequence>
                labels(mlfpathsmulti[i], restrictmlftokeys, statelistpaths[i], wordmap, (map<string, size_t>*) NULL, htktimetoframe);      // label MLF
            // get the temp file name for the page file

            // Make sure 'msra::asr::htkmlfreader' type has a move constructor
            static_assert(std::is_move_constructible<msra::asr::htkmlfreader<msra::asr::htkmlfentry, msra::lattices::lattice::htkmlfwordsequence>>::value,
                "Type 'msra::asr::htkmlfreader' should be move constructible!");

            labelsmulti.push_back(std::move(labels));
        }

        if (!_wcsicmp(readMethod.c_str(), L"blockRandomize"))
        {
            // construct all the parameters we don't need, but need to be passed to the constructor...

            m_lattices.reset(new msra::dbn::latticesource(latticetocs, m_hset.getsymmap()));

            // now get the frame source. This has better randomization and doesn't create temp files
            m_frameSource.reset(new msra::dbn::Bundler(infilesmulti, labelsmulti, m_featDims, m_labelDims, numContextLeft, numContextRight, randomize, *m_lattices, m_latticeMap, true));
            m_frameSource->setverbosity(m_verbosity);
        }
        else
        {
            RuntimeError("readMethod must be 'rollingWindow' or 'blockRandomize'");
        }
    }

    void MonolithicTransformer::SetEpochConfiguration(const EpochConfiguration& config)
    {
        assert(config.workerRank < config.numberOfWorkers);
        assert((config.workerRank == 0) && (config.numberOfWorkers == 1));
        size_t requestedEpochSamples = config.totalSize;

        m_mbNumTimeSteps = config.minibatchSize;       // note: ignored in frame mode and full-sequence mode
        m_numSeqsPerMB = m_numSeqsPerMBForAllEpochs[config.index];

        // resize the arrays
        // These are sized to the requested number. If not all can be filled, it will still return this many, just with gaps.
        // In frame mode, m_numSeqsPerMB must be 1. However, the returned layout has one 1-frame sequence per frame.
        m_numFramesToProcess.assign(m_numSeqsPerMB, 0);
        m_numValidFrames.assign(m_numSeqsPerMB, 0);

        if ((m_numSeqsPerMB > 1))
        {
            LogicError("nbrUttsInEachRecurrentIter cannot be more than 1 in frame mode reading.");
        }

        size_t datapasses = 1;
        size_t totalFrames = m_frameSource->totalframes();

        size_t extraFrames = totalFrames%config.minibatchSize;
        size_t minibatches = totalFrames / config.minibatchSize;

        // if we are allowing partial minibatches, do nothing, and let it go through
        if (!m_partialMinibatch)
        {
            // we don't want any partial frames, so round total frames to be an even multiple of our mbSize
            if (totalFrames > config.minibatchSize)
                totalFrames -= extraFrames;

            if (requestedEpochSamples == requestDataSize)
            {
                requestedEpochSamples = totalFrames;
            }
            else if (minibatches > 0)   // if we have any full minibatches
            {
                // since we skip the extraFrames, we need to add them to the total to get the actual number of frames requested
                size_t sweeps = (requestedEpochSamples - 1) / totalFrames; // want the number of sweeps we will skip the extra, so subtract 1 and divide
                requestedEpochSamples += extraFrames*sweeps;
            }
        }
        else if (requestedEpochSamples == requestDataSize)
        {
            requestedEpochSamples = totalFrames;
        }

        m_mbiter.reset(new msra::dbn::minibatchiterator(*m_frameSource, config.index, requestedEpochSamples, 1, config.workerRank, config.numberOfWorkers, datapasses));
        // Advance the MB iterator until we find some data or reach the end of epoch
        while ((m_mbiter->currentmbframes() == 0) && *m_mbiter)
        {
            (*m_mbiter)++;
        }

        m_noData = false;
        if (!(*m_mbiter))
            m_noData = true;
    }

    std::vector<InputDescriptionPtr> MonolithicTransformer::getInputs() const
    {
        return m_inputs;
    }

    std::map<InputId, Sequence> MonolithicTransformer::getNextSequence()
    {
        if (m_noData)
        {
            return std::map<InputId, Sequence>();
        }

        std::map<size_t, Sequence> result;
        for (auto it = m_featureNameToIdMap.begin(); it != m_featureNameToIdMap.end(); ++it)
        {
            Sequence r;
            size_t id = m_featureNameToIdMap[it->first];

            // eldak: leak here.
            const msra::dbn::matrixstripe featOri = m_mbiter->frames(id);
            const size_t dimensions = featOri.rows();
            const void* tmp = &featOri(0, 0);

            r.numberOfFrames = 1;
            r.frameDescription = &m_featureFrameDescriptions[id];

            // eldak: leak leak leak. who is responsible for clearing this? who does caching?
            void* buffer = nullptr;
            if (m_elementSize == sizeof(float))
            {
                buffer = new float[featOri.rows()];
            }
            else
            {
                buffer = new double[featOri.rows()];
            }

            memset(buffer, 0, m_elementSize * dimensions);
            memcpy_s(buffer, m_elementSize * dimensions, tmp, m_elementSize * dimensions);
            r.data = buffer;

            result.insert(std::make_pair(m_nameToId[it->first], r));
        }

        for (auto it = m_labelNameToIdMap.begin(); it != m_labelNameToIdMap.end(); ++it)
        {
            Sequence r;
            size_t id = m_labelNameToIdMap[it->first];
            size_t dim = m_labelNameToDimMap[it->first];

            const vector<size_t>& uids = m_mbiter->labels(id);

            // eldak: leak here.
            if (m_elementSize == sizeof(float))
            {
                float* tmp = new float[dim];
                memset(tmp, 0, m_elementSize * dim);
                tmp[uids[0]] = 1;
                r.data = tmp;
                r.numberOfFrames = 1;
                r.frameDescription = &m_labelFrameDescriptions[id];
            }
            else
            {
                double* tmp = new double[dim];
                tmp[uids[0]] = 1;
                r.data = tmp;
                r.numberOfFrames = 1;
                r.frameDescription = &m_labelFrameDescriptions[id];
            }
            result.insert(std::make_pair(m_nameToId[it->first], r));
        }

        // Advance the MB iterator until we find some data or reach the end of epoch
        ScopeTimer mbIterAdvancementTimer(m_verbosity, "Time to advance mbiter = %.8g\n");

        do
        {
            (*m_mbiter)++;
        }
        while ((m_mbiter->currentmbframes() == 0) && *m_mbiter);

        if (!(*m_mbiter))
            m_noData = true;

        return result;
    }
}}}
