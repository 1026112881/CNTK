//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#include "stdafx.h"
#include "Bundler.h"
#include "ConfigHelper.h"

namespace Microsoft { namespace MSR { namespace CNTK {

Bundler::Bundler(
    const ConfigParameters& readerConfig,
    IDataDeserializerPtr driver,
    std::vector<IDataDeserializerPtr> deserializers)
    : m_deserializers(deserializers), m_driver(driver)
{
    UNREFERENCED_PARAMETER(readerConfig);
    std::vector<StreamDescriptionPtr> streams;
    for (auto d : deserializers)
    {
        for (auto i : d->GetStreamDescriptions())
        {
            StreamDescriptionPtr stream = std::make_shared<StreamDescription>(*i);
            stream->m_id = streams.size();
            streams.push_back(stream);
        }
    }

    m_streams = streams;
    CreateSequenceDescriptions();
}

void Bundler::CreateSequenceDescriptions()
{
    m_sequenceToSequence.resize(m_deserializers.size());
    m_sequenceToChunk.resize(m_deserializers.size());
    m_sequenceDescriptions.reserve(m_driver->GetSequenceDescriptions().size());

    size_t maxNumberOfSequences = m_driver->GetSequenceDescriptions().size();
    for (int i = 0; i < m_deserializers.size(); ++i)
    {
        m_sequenceToSequence[i].resize(maxNumberOfSequences);
        m_sequenceToChunk[i].resize(maxNumberOfSequences);
    }

    size_t previousChunk = SIZE_MAX;
    size_t currentMapping = 0;
    for (int i = 0; i < m_driver->GetSequenceDescriptions().size(); ++i)
    {
        auto sequenceDescription = m_driver->GetSequenceDescriptions()[i];

        bool isValid = true;
        for (int j = 1; j < m_deserializers.size(); ++j)
        {
            auto description = m_deserializers[j]->GetSequenceDescriptionByKey(sequenceDescription->m_key);
            if (!description->m_isValid)
            {
                isValid = false;
                break;
            }

            m_sequenceToChunk[j][currentMapping] = description->m_chunkId;
            m_sequenceToSequence[j][currentMapping] = description->m_id;
        }

        m_sequenceToChunk[0][currentMapping] = sequenceDescription->m_chunkId;
        m_sequenceToSequence[0][currentMapping] = sequenceDescription->m_id;

        if (isValid)
        {
            if (sequenceDescription->m_chunkId != previousChunk)
            {
                m_chunkOffsets.push_back(m_sequenceDescriptions.size());
                previousChunk = sequenceDescription->m_chunkId;
            }

            m_sequenceDescriptions.push_back(*sequenceDescription);
            m_sequenceDescriptions.back().m_id = m_sequenceDescriptions.size() - 1;
            m_sequenceToSequence[0][currentMapping] = sequenceDescription->m_id;
            currentMapping++;
        }
    }

    for (int i = 0; i < m_deserializers.size(); ++i)
    {
        m_sequenceToSequence.resize(currentMapping);
    }

    // Last
    m_chunkOffsets.push_back(m_sequenceDescriptions.size());

    m_sequences.resize(m_sequenceDescriptions.size());
    for (int k = 0; k < m_sequenceDescriptions.size(); ++k)
    {
        m_sequences[k] = &m_sequenceDescriptions[k];
    }
}

class BundlingChunk : public Chunk // TODO tune implementation?
{
    size_t m_numberOfInputs;
    Bundler* m_parent;
    size_t m_chunkId;
    size_t m_sequenceEnd;
    std::vector<std::vector<ChunkPtr>> m_innerChunks;

public:
    BundlingChunk(size_t numberOfInputs, Bundler* parent, size_t chunkId)
        : m_numberOfInputs(numberOfInputs), m_parent(parent), m_chunkId(chunkId)
    {
        size_t numberOfSequences = m_parent->m_chunkOffsets[chunkId + 1] - m_parent->m_chunkOffsets[chunkId];
        m_innerChunks.resize(numberOfSequences);

        int innerIndex = 0;
        for (size_t sequenceId = m_parent->m_chunkOffsets[chunkId]; sequenceId < m_parent->m_chunkOffsets[chunkId + 1]; ++sequenceId, ++innerIndex)
        {
            m_innerChunks[innerIndex].resize(m_parent->m_deserializers.size());
            for (size_t i = 0; i < m_parent->m_deserializers.size(); ++i)
            {
                size_t innerChunkId = m_parent->m_sequenceToChunk[i][sequenceId];
                m_innerChunks[innerIndex][i] = m_parent->m_deserializers[i]->GetChunk(innerChunkId);
            }
        }
    }

    virtual std::vector<SequenceDataPtr> GetSequence(const size_t& sequenceId) override
    {
        size_t index = sequenceId - m_parent->m_chunkOffsets[m_chunkId];
        const auto& chunks = m_innerChunks[index];
        std::vector<SequenceDataPtr> result;
        result.reserve(m_numberOfInputs);

        for (int i = 0; i < chunks.size(); ++i)
        {
            auto sequences = chunks[i]->GetSequence(m_parent->m_sequenceToSequence[i][sequenceId]);
            result.insert(result.end(), sequences.begin(), sequences.end());
        }

        return result;
    }
};

ChunkPtr Bundler::GetChunk(size_t chunkId)
{
    return std::make_shared<BundlingChunk>(m_streams.size(), this, chunkId); //TODO why so slow?
}

const SequenceDescriptions& Bundler::GetSequenceDescriptions() const
{
    return m_sequences;
}

std::vector<StreamDescriptionPtr> Bundler::GetStreamDescriptions() const
{
    return m_streams;
}

const SequenceDescription* Bundler::GetSequenceDescriptionByKey(const KeyType&)
{
    throw std::logic_error("Not implemented");
}

}}}
