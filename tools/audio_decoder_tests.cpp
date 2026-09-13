#include "audio_decoder.h"
#include "audio_timetag.h"

#include <climits>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <errno.h>
#include <unistd.h>
#endif
#include <stdint.h>
#include <limits>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace
{
	int Failures = 0;

	void Check (bool condition, const char *name)
	{
		if (!condition)
		{
			fprintf (stderr, "FAILED: %s\n", name);
			++Failures;
		}
	}

	void AppendLE16 (std::vector<unsigned char> &bytes, unsigned int value)
	{
		bytes.push_back ((unsigned char)value);
		bytes.push_back ((unsigned char)(value >> 8));
	}

	void AppendLE32 (std::vector<unsigned char> &bytes, unsigned int value)
	{
		AppendLE16 (bytes, value & 0xffff);
		AppendLE16 (bytes, value >> 16);
	}

	void AppendLE64 (std::vector<unsigned char> &bytes, unsigned long long value)
	{
		AppendLE32 (bytes, (unsigned int)value);
		AppendLE32 (bytes, (unsigned int)(value >> 32));
	}

	unsigned int OggCRC (const unsigned char *data, std::size_t bytes)
	{
		unsigned int crc = 0;
		for (std::size_t index = 0; index < bytes; ++index)
		{
			crc ^= (unsigned int)data[index] << 24;
			for (unsigned int bit = 0; bit < 8; ++bit)
			{
				crc = (crc << 1) ^ ((crc & 0x80000000U) != 0 ? 0x04c11db7U : 0);
			}
		}
		return crc;
	}

	bool AppendOggPacketPage (std::vector<unsigned char> *output, const std::vector<unsigned char> &packet, unsigned int headerType, unsigned long long granulePosition, unsigned int serialNumber, unsigned int sequenceNumber)
	{
		std::vector<unsigned char> lacing;
		std::size_t remaining = packet.size ();
		std::size_t pageStart;
		unsigned int crc;
		while (remaining >= 255)
		{
			lacing.push_back (255);
			remaining -= 255;
		}
		lacing.push_back ((unsigned char)remaining);
		if (lacing.size () > 255)
		{
			return false;
		}
		pageStart = output->size ();
		output->insert (output->end (), "OggS", "OggS" + 4);
		output->push_back (0);
		output->push_back ((unsigned char)headerType);
		AppendLE64 (*output, granulePosition);
		AppendLE32 (*output, serialNumber);
		AppendLE32 (*output, sequenceNumber);
		AppendLE32 (*output, 0);
		output->push_back ((unsigned char)lacing.size ());
		output->insert (output->end (), lacing.begin (), lacing.end ());
		output->insert (output->end (), packet.begin (), packet.end ());
		crc = OggCRC (&(*output)[pageStart], output->size () - pageStart);
		(*output)[pageStart + 22] = (unsigned char)crc;
		(*output)[pageStart + 23] = (unsigned char)(crc >> 8);
		(*output)[pageStart + 24] = (unsigned char)(crc >> 16);
		(*output)[pageStart + 25] = (unsigned char)(crc >> 24);
		return true;
	}

	void AppendBE16 (std::vector<unsigned char> &bytes, unsigned int value)
	{
		bytes.push_back ((unsigned char)(value >> 8));
		bytes.push_back ((unsigned char)value);
	}

	void AppendBE24 (std::vector<unsigned char> &bytes, unsigned int value)
	{
		bytes.push_back ((unsigned char)(value >> 16));
		bytes.push_back ((unsigned char)(value >> 8));
		bytes.push_back ((unsigned char)value);
	}

	void AppendBE64 (std::vector<unsigned char> &bytes, unsigned long long value)
	{
		AppendBE16 (bytes, (unsigned int)(value >> 48));
		AppendBE16 (bytes, (unsigned int)(value >> 32));
		AppendBE16 (bytes, (unsigned int)(value >> 16));
		AppendBE16 (bytes, (unsigned int)value);
	}

	struct FlacMetadataBlock
	{
		std::size_t Offset;
		std::size_t Bytes;
		unsigned char Type;
		bool IsLast;
	};

	struct FixtureFlacFrame
	{
		std::size_t Offset;
		std::size_t Bytes;
		unsigned int PCMFrames;
		const unsigned char *Header;
		std::size_t HeaderBytes;
	};

	bool ParseNativeFlacMetadata (const std::vector<unsigned char> &nativeFlac, std::vector<FlacMetadataBlock> *blocks, std::size_t *audioOffset)
	{
		std::size_t offset = 4;
		if (blocks == NULL || audioOffset == NULL || nativeFlac.size () < 8 || memcmp (&nativeFlac[0], "fLaC", 4) != 0)
		{
			return false;
		}
		blocks->clear ();
		while (offset <= nativeFlac.size () - 4)
		{
			FlacMetadataBlock block;
			unsigned int length = ((unsigned int)nativeFlac[offset + 1] << 16) | ((unsigned int)nativeFlac[offset + 2] << 8) | nativeFlac[offset + 3];
			if (length > nativeFlac.size () - offset - 4)
			{
				return false;
			}
			block.Offset = offset;
			block.Bytes = 4 + length;
			block.Type = nativeFlac[offset] & 0x7f;
			block.IsLast = (nativeFlac[offset] & 0x80) != 0;
			blocks->push_back (block);
			offset += block.Bytes;
			if (block.IsLast)
			{
				*audioOffset = offset;
				return offset < nativeFlac.size ();
			}
		}
		return false;
	}

	bool GetFixtureFlacFrames (const std::vector<unsigned char> &nativeFlac, std::size_t audioOffset, std::vector<FixtureFlacFrame> *frames)
	{
		static const unsigned char firstFrameHeader[] = { 0xff, 0xf8, 0x94, 0x08, 0x00, 0x20 };
		static const unsigned char secondFrameHeader[] = { 0xff, 0xf8, 0x64, 0x08, 0x01, 0x10, 0x86 };
		static const FixtureFlacFrame fixtureFrames[] =
		{
			{ 0, 546, 512, firstFrameHeader, sizeof (firstFrameHeader) },
			{ 546, 52, 17, secondFrameHeader, sizeof (secondFrameHeader) }
		};
		if (frames == NULL || audioOffset > nativeFlac.size () || nativeFlac.size () - audioOffset != 598)
		{
			return false;
		}
		frames->clear ();
		for (std::size_t index = 0; index < sizeof (fixtureFrames) / sizeof (fixtureFrames[0]); ++index)
		{
			const FixtureFlacFrame &frame = fixtureFrames[index];
			if (frame.Offset > nativeFlac.size () - audioOffset || frame.Bytes > nativeFlac.size () - audioOffset - frame.Offset || frame.HeaderBytes > frame.Bytes || memcmp (&nativeFlac[audioOffset + frame.Offset], frame.Header, frame.HeaderBytes) != 0)
			{
				return false;
			}
			frames->push_back (frame);
		}
		return true;
	}

	bool MakeFlacWithSeektable (const std::vector<unsigned char> &nativeFlac, std::vector<unsigned char> *seektableFlac)
	{
		std::vector<FlacMetadataBlock> blocks;
		std::size_t audioOffset;
		std::size_t commentIndex = (std::size_t)-1;
		if (seektableFlac == NULL || !ParseNativeFlacMetadata (nativeFlac, &blocks, &audioOffset) || blocks.empty () || blocks[0].Type != 0 || blocks[0].Bytes != 38)
		{
			return false;
		}
		for (std::size_t index = 1; index < blocks.size (); ++index)
		{
			if (blocks[index].Type == 4)
			{
				commentIndex = index;
				break;
			}
		}
		if (commentIndex == (std::size_t)-1)
		{
			return false;
		}
		seektableFlac->clear ();
		seektableFlac->insert (seektableFlac->end (), "fLaC", "fLaC" + 4);
		seektableFlac->insert (seektableFlac->end (), nativeFlac.begin () + blocks[0].Offset, nativeFlac.begin () + blocks[0].Offset + blocks[0].Bytes);
		(*seektableFlac)[4] &= 0x7f;
		seektableFlac->insert (seektableFlac->end (), nativeFlac.begin () + blocks[commentIndex].Offset, nativeFlac.begin () + blocks[commentIndex].Offset + blocks[commentIndex].Bytes);
		(*seektableFlac)[4 + blocks[0].Bytes] &= 0x7f;
		seektableFlac->push_back (0x83);
		AppendBE24 (*seektableFlac, 18);
		AppendBE64 (*seektableFlac, 0);
		AppendBE64 (*seektableFlac, 0);
		AppendBE16 (*seektableFlac, 512);
		seektableFlac->insert (seektableFlac->end (), nativeFlac.begin () + audioOffset, nativeFlac.end ());
		return true;
	}

	bool MakeOggFlac (const std::vector<unsigned char> &nativeFlac, unsigned long long totalPCMFrames, std::vector<unsigned char> *oggFlac)
	{
		std::vector<unsigned char> mapping;
		std::vector<FlacMetadataBlock> blocks;
		std::vector<FixtureFlacFrame> frames;
		std::size_t audioOffset;
		unsigned int sequenceNumber = 0;
		unsigned long long granulePosition = 0;
		if (oggFlac == NULL || !ParseNativeFlacMetadata (nativeFlac, &blocks, &audioOffset) || !GetFixtureFlacFrames (nativeFlac, audioOffset, &frames) || blocks.empty () || blocks[0].Type != 0 || blocks[0].Bytes != 38 || blocks.size () - 1 > 0xffff)
		{
			return false;
		}
		mapping.push_back (0x7f);
		mapping.insert (mapping.end (), "FLAC", "FLAC" + 4);
		mapping.push_back (1);
		mapping.push_back (0);
		AppendBE16 (mapping, (unsigned int)(blocks.size () - 1));
		mapping.insert (mapping.end (), "fLaC", "fLaC" + 4);
		mapping.insert (mapping.end (), nativeFlac.begin () + blocks[0].Offset, nativeFlac.begin () + blocks[0].Offset + blocks[0].Bytes);
		oggFlac->clear ();
		if (!AppendOggPacketPage (oggFlac, mapping, 0x02, 0, 0x1a2b3c4dU, sequenceNumber++))
		{
			return false;
		}
		for (std::size_t index = 1; index < blocks.size (); ++index)
		{
			std::vector<unsigned char> metadata (nativeFlac.begin () + blocks[index].Offset, nativeFlac.begin () + blocks[index].Offset + blocks[index].Bytes);
			if (!AppendOggPacketPage (oggFlac, metadata, 0, 0, 0x1a2b3c4dU, sequenceNumber++))
			{
				return false;
			}
		}
		for (std::size_t index = 0; index < frames.size (); ++index)
		{
			const FixtureFlacFrame &frame = frames[index];
			const std::vector<unsigned char> audio (nativeFlac.begin () + audioOffset + frame.Offset, nativeFlac.begin () + audioOffset + frame.Offset + frame.Bytes);
			granulePosition += frame.PCMFrames;
			if (!AppendOggPacketPage (oggFlac, audio, index + 1 == frames.size () ? 0x04 : 0, granulePosition, 0x1a2b3c4dU, sequenceNumber++))
			{
				return false;
			}
		}
		return granulePosition == totalPCMFrames;
	}

	unsigned int ReadLE32 (const unsigned char *data)
	{
		return (unsigned int)data[0] | ((unsigned int)data[1] << 8) | ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
	}

	unsigned long long ReadLE64 (const unsigned char *data)
	{
		return (unsigned long long)ReadLE32 (data) | ((unsigned long long)ReadLE32 (data + 4) << 32);
	}

	unsigned int OggPageCRC (const unsigned char *data, std::size_t bytes)
	{
		unsigned int crc = 0;
		for (std::size_t index = 0; index < bytes; ++index)
		{
			unsigned char value = index >= 22 && index < 26 ? 0 : data[index];
			crc ^= (unsigned int)value << 24;
			for (unsigned int bit = 0; bit < 8; ++bit)
			{
				crc = (crc << 1) ^ ((crc & 0x80000000U) != 0 ? 0x04c11db7U : 0);
			}
		}
		return crc;
	}

	struct OggPacketPage
	{
		const unsigned char *Packet;
		std::size_t PacketBytes;
		unsigned char HeaderType;
		unsigned long long GranulePosition;
	};

	bool ReadOggPacketPage (const std::vector<unsigned char> &oggFlac, std::size_t *offset, unsigned int sequenceNumber, OggPacketPage *page)
	{
		std::size_t lacingBytes;
		std::size_t packetBytes = 0;
		std::size_t headerBytes;
		if (offset == NULL || page == NULL || *offset > oggFlac.size () || oggFlac.size () - *offset < 27 || memcmp (&oggFlac[*offset], "OggS", 4) != 0 || oggFlac[*offset + 4] != 0)
		{
			return false;
		}
		lacingBytes = oggFlac[*offset + 26];
		headerBytes = 27 + lacingBytes;
		if (lacingBytes == 0 || headerBytes > oggFlac.size () - *offset)
		{
			return false;
		}
		for (std::size_t laceIndex = 0; laceIndex < lacingBytes; ++laceIndex)
		{
			packetBytes += oggFlac[*offset + 27 + laceIndex];
		}
		if (packetBytes > oggFlac.size () - *offset - headerBytes || OggPageCRC (&oggFlac[*offset], headerBytes + packetBytes) != ReadLE32 (&oggFlac[*offset + 22]) || ReadLE32 (&oggFlac[*offset + 14]) != 0x1a2b3c4dU || ReadLE32 (&oggFlac[*offset + 18]) != sequenceNumber)
		{
			return false;
		}
		page->Packet = &oggFlac[*offset + headerBytes];
		page->PacketBytes = packetBytes;
		page->HeaderType = oggFlac[*offset + 5];
		page->GranulePosition = ReadLE64 (&oggFlac[*offset + 6]);
		*offset += headerBytes + packetBytes;
		return true;
	}

	bool ValidateOggFlacHeaderPage (const OggPacketPage &page, const std::vector<FlacMetadataBlock> &blocks, const std::vector<unsigned char> &nativeFlac)
	{
		return page.HeaderType == 0x02 && page.GranulePosition == 0 && page.PacketBytes == 51 && page.Packet[0] == 0x7f && memcmp (page.Packet + 1, "FLAC", 4) == 0 && page.Packet[5] == 1 && page.Packet[6] == 0 && page.Packet[7] == 0 && page.Packet[8] == blocks.size () - 1 && memcmp (page.Packet + 9, "fLaC", 4) == 0 && memcmp (page.Packet + 13, &nativeFlac[blocks[0].Offset], blocks[0].Bytes) == 0 && (page.Packet[13] & 0x80) == 0;
	}

	bool ValidateOggFlacMetadataPage (const OggPacketPage &page, const FlacMetadataBlock &block, std::size_t pageIndex, const std::vector<unsigned char> &nativeFlac)
	{
		return page.HeaderType == 0 && page.GranulePosition == 0 && page.PacketBytes == block.Bytes && memcmp (page.Packet, &nativeFlac[block.Offset], block.Bytes) == 0 && (pageIndex != 1 || (block.Type == 4 && !block.IsLast)) && ((page.Packet[0] & 0x80) != 0) == block.IsLast;
	}

	bool ValidateOggFlacAudioPage (const OggPacketPage &page, const FixtureFlacFrame &frame, std::size_t pageIndex, std::size_t pageCount, std::size_t nativeAudioOffset, const std::vector<unsigned char> &nativeFlac, unsigned long long *granulePosition)
	{
		*granulePosition += frame.PCMFrames;
		return page.HeaderType == (pageIndex + 1 == pageCount ? 0x04 : 0) && page.GranulePosition == *granulePosition && page.PacketBytes == frame.Bytes && memcmp (page.Packet, &nativeFlac[nativeAudioOffset + frame.Offset], frame.Bytes) == 0;
	}

	bool ValidateOggFlac (const std::vector<unsigned char> &oggFlac, const std::vector<unsigned char> &nativeFlac, unsigned long long totalPCMFrames)
	{
		std::vector<FlacMetadataBlock> blocks;
		std::vector<FixtureFlacFrame> frames;
		std::size_t nativeAudioOffset;
		std::size_t offset = 0;
		unsigned long long granulePosition = 0;
		if (!ParseNativeFlacMetadata (nativeFlac, &blocks, &nativeAudioOffset) || !GetFixtureFlacFrames (nativeFlac, nativeAudioOffset, &frames) || blocks.size () < 2)
		{
			return false;
		}
		for (std::size_t pageIndex = 0; pageIndex < blocks.size () + frames.size (); ++pageIndex)
		{
			OggPacketPage page;
			if (!ReadOggPacketPage (oggFlac, &offset, (unsigned int)pageIndex, &page))
			{
				return false;
			}
			if (pageIndex == 0)
			{
				if (!ValidateOggFlacHeaderPage (page, blocks, nativeFlac))
				{
					return false;
				}
			}
			else if (pageIndex < blocks.size ())
			{
				const FlacMetadataBlock &block = blocks[pageIndex];
				if (!ValidateOggFlacMetadataPage (page, block, pageIndex, nativeFlac))
				{
					return false;
				}
			}
			else
			{
				const FixtureFlacFrame &frame = frames[pageIndex - blocks.size ()];
				if (!ValidateOggFlacAudioPage (page, frame, pageIndex, blocks.size () + frames.size (), nativeAudioOffset, nativeFlac, &granulePosition))
				{
					return false;
				}
			}
		}
		return granulePosition == totalPCMFrames && offset == oggFlac.size ();
	}

	std::vector<unsigned char> MakeWave (unsigned int format)
	{
		std::vector<unsigned char> bytes;
		bytes.insert (bytes.end (), "RIFF", "RIFF" + 4);
		AppendLE32 (bytes, 28);
		bytes.insert (bytes.end (), "WAVE", "WAVE" + 4);
		bytes.insert (bytes.end (), "fmt ", "fmt " + 4);
		AppendLE32 (bytes, 16);
		AppendLE16 (bytes, format);
		AppendLE16 (bytes, 2);
		AppendLE32 (bytes, 44100);
		AppendLE32 (bytes, 176400);
		AppendLE16 (bytes, 4);
		AppendLE16 (bytes, 16);
		return bytes;
	}

	std::vector<unsigned char> MakeDecodableWave (unsigned int channels)
	{
		const short samples[] = { 100, -100, 200, -200, 300, -300 };
		std::vector<unsigned char> bytes;
		std::size_t sampleCount = channels == 1 ? 3 : 6;
		bytes.insert (bytes.end (), "RIFF", "RIFF" + 4);
		AppendLE32 (bytes, (unsigned int)(36 + sampleCount * sizeof (short)));
		bytes.insert (bytes.end (), "WAVEfmt ", "WAVEfmt " + 8);
		AppendLE32 (bytes, 16);
		AppendLE16 (bytes, 1);
		AppendLE16 (bytes, channels);
		AppendLE32 (bytes, 8000);
		AppendLE32 (bytes, 8000 * channels * sizeof (short));
		AppendLE16 (bytes, channels * sizeof (short));
		AppendLE16 (bytes, 16);
		bytes.insert (bytes.end (), "data", "data" + 4);
		AppendLE32 (bytes, (unsigned int)(sampleCount * sizeof (short)));
		for (std::size_t index = 0; index < sampleCount; ++index)
		{
			AppendLE16 (bytes, (unsigned short)samples[index]);
		}
		return bytes;
	}

	std::vector<unsigned char> MakeReaderWave (unsigned int format, unsigned int bitsPerSample, unsigned long long sampleBits, std::size_t dataBytes)
	{
		std::vector<unsigned char> bytes;
		std::vector<unsigned char> sample;
		unsigned int bytesPerSample = bitsPerSample / 8;
		bytes.insert (bytes.end (), "RIFF", "RIFF" + 4);
		AppendLE32 (bytes, (unsigned int)(36 + dataBytes));
		bytes.insert (bytes.end (), "WAVEfmt ", "WAVEfmt " + 8);
		AppendLE32 (bytes, 16);
		AppendLE16 (bytes, format);
		AppendLE16 (bytes, 1);
		AppendLE32 (bytes, 8000);
		AppendLE32 (bytes, 8000 * bytesPerSample);
		AppendLE16 (bytes, bytesPerSample);
		AppendLE16 (bytes, bitsPerSample);
		bytes.insert (bytes.end (), "data", "data" + 4);
		AppendLE32 (bytes, (unsigned int)dataBytes);
		for (unsigned int index = 0; index < bytesPerSample; ++index)
		{
			sample.push_back ((unsigned char)(sampleBits >> (index * 8)));
		}
		for (std::size_t index = 0; index < dataBytes; ++index)
		{
			bytes.push_back (sample[index % sample.size ()]);
		}
		return bytes;
	}

	unsigned long FixtureProcessId ()
	{
#ifdef _WIN32
		return (unsigned long)_getpid ();
#else
		return (unsigned long)getpid ();
#endif
	}

	int OpenFixtureFile (const char *name)
	{
#ifdef _WIN32
		return _open (name, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
		return open (name, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
	}

	bool WriteFixtureBytes (int fileDescriptor, const std::vector<unsigned char> &bytes)
	{
		if (bytes.empty ())
		{
			return true;
		}
#ifdef _WIN32
		if (bytes.size () > UINT_MAX)
		{
			return false;
		}
		{
			int bytesWritten = _write (fileDescriptor, &bytes[0], (unsigned int)bytes.size ());
			return bytesWritten >= 0 && (unsigned int)bytesWritten == (unsigned int)bytes.size ();
		}
#else
		const unsigned char *data = &bytes[0];
		std::size_t remaining = bytes.size ();
		while (remaining > 0)
		{
			ssize_t bytesWritten = write (fileDescriptor, data, remaining);
			if (bytesWritten <= 0)
			{
				return false;
			}
			data += bytesWritten;
			remaining -= (std::size_t)bytesWritten;
		}
		return true;
#endif
	}

	void CloseFixtureFile (int fileDescriptor)
	{
#ifdef _WIN32
		_close (fileDescriptor);
#else
		close (fileDescriptor);
#endif
	}

	bool FixtureFileAlreadyExists ()
	{
		return errno == EEXIST;
	}

	bool WriteUniqueTemporaryFixture (const std::vector<unsigned char> &bytes, const char *kind, const char *directory, unsigned int *sequence, std::string *path)
	{
		int fileDescriptor;
		char name[128];
		for (unsigned int attempt = 0; attempt < 64; ++attempt)
		{
			if (directory != NULL)
			{
				sprintf (name, "%s/audio_decoder_fallback_%lu_%u_%s.wav", directory, FixtureProcessId (), ++*sequence, kind);
			}
			else
			{
				sprintf (name, "audio_decoder_fallback_%lu_%u_%s.wav", FixtureProcessId (), ++*sequence, kind);
			}
			fileDescriptor = OpenFixtureFile (name);
			if (fileDescriptor >= 0)
			{
				break;
			}
			if (!FixtureFileAlreadyExists ())
			{
				return false;
			}
		}
		if (fileDescriptor < 0)
		{
			return false;
		}
		if (!WriteFixtureBytes (fileDescriptor, bytes))
		{
			CloseFixtureFile (fileDescriptor);
			remove (name);
			return false;
		}
		CloseFixtureFile (fileDescriptor);
		try
		{
			*path = name;
		}
		catch (...)
		{
			remove (name);
			throw;
		}
		return true;
	}

	class TemporaryFixtureFiles
	{
	public:
		TemporaryFixtureFiles () : Sequence (0)
		{
		}

		~TemporaryFixtureFiles ()
		{
			for (std::vector<std::string>::const_iterator path = Paths.begin (); path != Paths.end (); ++path)
			{
				remove (path->c_str ());
			}
		}

		bool Write (const std::vector<unsigned char> &bytes, const char *kind, std::string *path)
		{
			std::string createdPath;
			if (!WriteUniqueTemporaryFixture (bytes, kind, NULL, &Sequence, &createdPath))
			{
				return false;
			}
			try
			{
				Paths.push_back (createdPath);
			}
			catch (...)
			{
				remove (createdPath.c_str ());
				throw;
			}
			*path = createdPath;
			return true;
		}

	private:
		std::vector<std::string> Paths;
		unsigned int Sequence;
	};

	std::vector<unsigned char> MakeMpeg ()
	{
		const unsigned char header[] = { 0xff, 0xfb, 0x90, 0 };
		std::vector<unsigned char> bytes (417, 0);
		memcpy (&bytes[0], header, sizeof (header));
		return bytes;
	}

	std::vector<unsigned char> MakeId3Mpeg (std::size_t tagLength = 0, bool withFooter = false)
	{
		std::vector<unsigned char> bytes;
		std::vector<unsigned char> tag (tagLength, 0);
		bytes.insert (bytes.end (), "ID3", "ID3" + 3);
		bytes.push_back (4);
		bytes.push_back (0);
		bytes.push_back (withFooter ? 0x10 : 0);
		bytes.push_back ((unsigned char)((tagLength >> 21) & 0x7f));
		bytes.push_back ((unsigned char)((tagLength >> 14) & 0x7f));
		bytes.push_back ((unsigned char)((tagLength >> 7) & 0x7f));
		bytes.push_back ((unsigned char)(tagLength & 0x7f));
		if (tagLength >= 11)
		{
			tag[0] = 'T';
			tag[1] = 'I';
			tag[2] = 'T';
			tag[3] = '2';
			tag[7] = 1;
			tag[10] = 'x';
		}
		bytes.insert (bytes.end (), tag.begin (), tag.end ());
		if (withFooter)
		{
			bytes.insert (bytes.end (), "3DI", "3DI" + 3);
			bytes.push_back (4);
			bytes.push_back (0);
			bytes.push_back (0x10);
			bytes.push_back ((unsigned char)((tagLength >> 21) & 0x7f));
			bytes.push_back ((unsigned char)((tagLength >> 14) & 0x7f));
			bytes.push_back ((unsigned char)((tagLength >> 7) & 0x7f));
			bytes.push_back ((unsigned char)(tagLength & 0x7f));
		}
		std::vector<unsigned char> mpeg = MakeMpeg ();
		bytes.insert (bytes.end (), mpeg.begin (), mpeg.end ());
		return bytes;
	}

	std::vector<unsigned char> MakeOgg (const char *identifier, std::size_t length)
	{
		std::vector<unsigned char> bytes (28, 0);
		bytes[0] = 'O';
		bytes[1] = 'g';
		bytes[2] = 'g';
		bytes[3] = 'S';
		bytes[26] = 1;
		bytes[27] = (unsigned char)length;
		bytes.insert (bytes.end (), identifier, identifier + length);
		return bytes;
	}

	std::string FixturePath (const char *name)
	{
		return std::string (AUDIO_DECODER_TESTDATA_DIR) + "/" + name;
	}

	bool ReadFileBytes (const char *path, std::vector<unsigned char> *bytes)
	{
		FILE *file = fopen (path, "rb");
		long length;
		if (file == NULL)
		{
			return false;
		}
		if (fseek (file, 0, SEEK_END) != 0 || (length = ftell (file)) < 0 || fseek (file, 0, SEEK_SET) != 0)
		{
			fclose (file);
			return false;
		}
		bytes->assign ((std::size_t)length, 0);
		if (!bytes->empty () && fread (&(*bytes)[0], 1, bytes->size (), file) != bytes->size ())
		{
			fclose (file);
			return false;
		}
		fclose (file);
		return true;
	}

	bool EqualDecodedPCM (const AudioDecodedPCM16 &left, const AudioDecodedPCM16 &right)
	{
		return left.SampleRate == right.SampleRate && left.Channels == right.Channels && left.Samples == right.Samples;
	}

	unsigned long long HashPCM16 (const AudioDecodedPCM16 &decoded)
	{
		unsigned long long hash = 1469598103934665603ULL;
		for (std::size_t index = 0; index < decoded.Samples.size (); ++index)
		{
			unsigned short sample = (unsigned short)decoded.Samples[index];
			hash = (hash ^ (sample & 0xff)) * 1099511628211ULL;
			hash = (hash ^ (sample >> 8)) * 1099511628211ULL;
		}
		return hash;
	}

	void TestDecoderExactEOF (AudioDecoder *decoder, short *frames, std::size_t *framesRead, unsigned long long expectedFrames, const char *name)
	{
		Check (decoder->SeekFrame (expectedFrames) == AUDIO_DECODER_SEEK_OK && decoder->TellFrame () == expectedFrames && decoder->ReadFrames (frames, 1, framesRead) == AUDIO_DECODER_EOF && *framesRead == 0, name);
		Check (decoder->SeekFrame (expectedFrames + 1) == AUDIO_DECODER_SEEK_ERROR && decoder->TellFrame () == expectedFrames, name);
	}

	void TestDecoderReadAndSeek (const char *path, unsigned long long expectedFrames, const char *name)
	{
		AudioFileSource source;
		AudioDecoder *decoder = NULL;
		AudioProbeResult probe;
		short first[2] = { 0, 0 };
		short repeated[2] = { 0, 0 };
		short frames[514] = { 0 };
		std::size_t framesRead = 0;
		std::size_t totalFrames = 1;
		bool failed = false;
		bool sawPartialRead = false;
		Check (source.Open (path) == AUDIO_SOURCE_OK, name);
		probe = ProbeAudioFormat (source);
		if (CreateAudioDecoder (source, probe, &decoder) != AUDIO_DECODE_OK || decoder == NULL)
		{
			Check (false, name);
			return;
		}
		Check (decoder->ReadFrames (first, 1, &framesRead) == AUDIO_DECODER_DATA && framesRead == 1, name);
		for (;;)
		{
			AudioDecoderReadStatus status = decoder->ReadFrames (frames, 257, &framesRead);
			if (status == AUDIO_DECODER_EOF)
			{
				break;
			}
			if (status != AUDIO_DECODER_DATA || framesRead == 0 || framesRead > 257 || totalFrames > 1000000)
			{
				failed = true;
				break;
			}
			sawPartialRead = sawPartialRead || framesRead < 257;
			totalFrames += framesRead;
		}
		Check (!failed && decoder->ReadFrames (frames, 1, &framesRead) == AUDIO_DECODER_EOF && framesRead == 0, name);
		Check (totalFrames == expectedFrames && sawPartialRead, name);
		TestDecoderExactEOF (decoder, frames, &framesRead, expectedFrames, name);
		Check (decoder->SeekFrame (0) == AUDIO_DECODER_SEEK_OK && decoder->SeekFrame (0) == AUDIO_DECODER_SEEK_OK, name);
		Check (decoder->ReadFrames (repeated, 1, &framesRead) == AUDIO_DECODER_DATA && framesRead == 1 && repeated[0] == first[0] && repeated[1] == first[1], name);
		delete decoder;
	}

	void TestFixtureParity (const char *filename, AudioFormat expectedFormat, unsigned int expectedRate, unsigned int expectedChannels, unsigned long long expectedFrames, unsigned long long expectedPCMHash, const char *name)
	{
		std::string path = FixturePath (filename);
		const char *slicePath = "audio_decoder_tests_fixture_slice.bin";
		std::vector<unsigned char> bytes;
		AudioMemorySource memory;
		AudioFileSource file;
		AudioFileSource slice;
		AudioDecodedPCM16 memoryPCM;
		AudioDecodedPCM16 filePCM;
		AudioDecodedPCM16 slicePCM;
		AudioProbeResult probe;
		FILE *wrapper;
		Check (ReadFileBytes (path.c_str (), &bytes) && !bytes.empty (), name);
		if (bytes.empty ())
		{
			return;
		}
		memory.Assign (&bytes[0], bytes.size ());
		probe = ProbeAudioFormat (memory);
		Check (probe.Status == AUDIO_PROBE_RECOGNIZED && probe.Format == expectedFormat, name);
		Check (DecodeAudioToPCM16 (memory, probe, &memoryPCM) == AUDIO_DECODE_OK, name);
		Check (memoryPCM.SampleRate == expectedRate && memoryPCM.Channels == expectedChannels && memoryPCM.Samples.size () == expectedFrames * expectedChannels, name);
		Check (HashPCM16 (memoryPCM) == expectedPCMHash, name);
		Check (file.Open (path.c_str ()) == AUDIO_SOURCE_OK, name);
		probe = ProbeAudioFormat (file);
		Check (DecodeAudioToPCM16 (file, probe, &filePCM) == AUDIO_DECODE_OK && EqualDecodedPCM (memoryPCM, filePCM), name);
		wrapper = fopen (slicePath, "wb");
		if (wrapper == NULL)
		{
			Check (false, name);
			return;
		}
		fwrite ("head", 1, 4, wrapper);
		fwrite (&bytes[0], 1, bytes.size (), wrapper);
		fwrite ("tail", 1, 4, wrapper);
		fclose (wrapper);
		Check (slice.OpenSlice (slicePath, 4, bytes.size ()) == AUDIO_SOURCE_OK, name);
		probe = ProbeAudioFormat (slice);
		Check (DecodeAudioToPCM16 (slice, probe, &slicePCM) == AUDIO_DECODE_OK && EqualDecodedPCM (memoryPCM, slicePCM), name);
		slice.Close ();
		remove (slicePath);
		TestDecoderReadAndSeek (path.c_str (), expectedFrames, name);
	}

	void TestTruncatedFixture (const char *filename, const char *name)
	{
		std::vector<unsigned char> bytes;
		AudioMemorySource source;
		AudioDecodedPCM16 decoded;
		std::string path = FixturePath (filename);
		Check (ReadFileBytes (path.c_str (), &bytes) && bytes.size () > 1, name);
		if (bytes.size () <= 1)
		{
			return;
		}
		bytes.resize (bytes.size () < 64 ? bytes.size () - 1 : 64);
		source.Assign (&bytes[0], bytes.size ());
		Check (DecodeAudioToPCM16 (source, ProbeAudioFormat (source), &decoded) != AUDIO_DECODE_OK, name);
	}

	class FailingSource : public AudioDataSource
	{
	public:
		FailingSource (const std::vector<unsigned char> &data, bool failRead, bool failRestore, std::size_t failReadOnCall = 0)
			: Data (data), Position (0), FailRead (failRead), FailRestore (failRestore), FailReadOnCall (failReadOnCall), ReadCalls (0)
		{
		}

		AudioSourceStatus Read (void *data, std::size_t bytes, std::size_t *bytesRead)
		{
			if (bytesRead == NULL || (data == NULL && bytes != 0))
			{
				return AUDIO_SOURCE_INVALID_ARGUMENT;
			}
			*bytesRead = 0;
			++ReadCalls;
			if (FailRead || ReadCalls == FailReadOnCall)
			{
				return AUDIO_SOURCE_IO_ERROR;
			}
			std::size_t available = Data.size () - Position;
			std::size_t count = bytes < available ? bytes : available;
			if (count != 0)
			{
				memcpy (data, &Data[Position], count);
				Position += count;
				*bytesRead = count;
			}
			return count == bytes ? AUDIO_SOURCE_OK : AUDIO_SOURCE_EOF;
		}

		AudioSourceStatus Seek (AudioSeekOrigin origin, long long offset)
		{
			if (FailRestore && origin == AUDIO_SEEK_BEGIN && offset == 0)
			{
				return AUDIO_SOURCE_IO_ERROR;
			}
			if (origin != AUDIO_SEEK_BEGIN || offset < 0 || (unsigned long long)offset > Data.size ())
			{
				return AUDIO_SOURCE_RANGE_ERROR;
			}
			Position = (std::size_t)offset;
			return AUDIO_SOURCE_OK;
		}

		std::size_t Tell () const
		{
			return Position;
		}

		std::size_t GetLength () const
		{
			return Data.size ();
		}

	private:
		std::vector<unsigned char> Data;
		std::size_t Position;
		bool FailRead;
		bool FailRestore;
		std::size_t FailReadOnCall;
		std::size_t ReadCalls;
	};

	class ContractDecoder : public AudioDecoder
	{
	public:
		ContractDecoder () : Position (0), HasData (true) {}

		AudioFormat GetEncodedFormat () const { return AUDIO_FORMAT_MPEG; }
		const AudioPCMMetadata &GetMetadata () const { return Metadata; }
		unsigned int GetNativeSampleRate () const { return 48000; }
		unsigned int GetOutputChannels () const { return 2; }
		bool GetTotalFrames (unsigned long long *frames) const { *frames = 1; return true; }
		unsigned long long TellFrame () const { return Position; }
		bool GetLoopRange (AudioFrameRange *range) const { range->Start = 0; range->End = 1; return true; }
		int GetDiagnosticCode () const { return 17; }
		const char *GetDiagnosticString () const { return "test diagnostic"; }
		AudioDecoderReadStatus ReadFrames (short *frames, std::size_t frameCount, std::size_t *framesRead)
		{
			if (frames == NULL || framesRead == NULL || frameCount == 0)
			{
				return AUDIO_DECODER_ERROR;
			}
			*framesRead = HasData ? 1 : 0;
			HasData = false;
			return *framesRead != 0 ? AUDIO_DECODER_DATA : AUDIO_DECODER_EOF;
		}
		AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
		{
			if (frame > 1)
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			Position = frame;
			return AUDIO_DECODER_SEEK_OK;
		}

	private:
		AudioPCMMetadata Metadata;
		unsigned long long Position;
		bool HasData;
	};

	void TestMemoryOwnership ()
	{
		AudioMemorySource source;
		{
			std::vector<unsigned char> input;
			input.push_back (1);
			input.push_back (2);
			input.push_back (3);
			Check (source.Assign (&input[0], input.size ()) == AUDIO_SOURCE_OK, "copied memory assigned");
		}
		unsigned char output[3] = { 0, 0, 0 };
		std::size_t bytesRead = 0;
		Check (source.Read (output, sizeof (output), &bytesRead) == AUDIO_SOURCE_OK && bytesRead == sizeof (output) && output[2] == 3, "copied memory survives caller lifetime");
	}

	void TestFileSourceBoundaries (const char *path)
	{
		AudioFileSource source;
		unsigned char output[8] = { 0, };
		std::size_t bytesRead = 0;
		Check (source.OpenSlice (path, 2, 5) == AUDIO_SOURCE_OK && source.GetLength () == 5, "file slice opens");
		Check (source.Read (output, sizeof (output), &bytesRead) == AUDIO_SOURCE_EOF && bytesRead == 5 && memcmp (output, "23456", 5) == 0, "file slice excludes suffix");
		Check (source.Seek (AUDIO_SEEK_BEGIN, 0) == AUDIO_SOURCE_OK, "seek begin");
		Check (source.Seek (AUDIO_SEEK_CURRENT, 2) == AUDIO_SOURCE_OK && source.Tell () == 2, "seek current forward");
		Check (source.Seek (AUDIO_SEEK_CURRENT, -1) == AUDIO_SOURCE_OK && source.Tell () == 1, "seek current backward");
		Check (source.Seek (AUDIO_SEEK_END, -1) == AUDIO_SOURCE_OK && source.Tell () == 4, "seek end backward");
		Check (source.Read (output, 1, &bytesRead) == AUDIO_SOURCE_OK && bytesRead == 1 && output[0] == '6', "file slice read at end boundary");
		Check (source.Read (output, 1, &bytesRead) == AUDIO_SOURCE_EOF && bytesRead == 0, "file slice EOF");
		Check (source.Seek (AUDIO_SEEK_BEGIN, -1) == AUDIO_SOURCE_RANGE_ERROR, "seek before slice rejected");
		Check (source.Seek (AUDIO_SEEK_END, 1) == AUDIO_SOURCE_RANGE_ERROR, "seek after slice rejected");
		Check (source.Seek (AUDIO_SEEK_BEGIN, std::numeric_limits<long long>::max ()) == AUDIO_SOURCE_RANGE_ERROR, "positive seek overflow rejected");
		Check (source.Seek (AUDIO_SEEK_END, std::numeric_limits<long long>::min ()) == AUDIO_SOURCE_RANGE_ERROR, "negative seek overflow rejected");
	}

	void TestFileFailures (const char *path)
	{
		AudioFileSource source;
		Check (source.Open ("audio_decoder_missing_input.bin") == AUDIO_SOURCE_IO_ERROR, "missing file is non-fatal");
		Check (source.OpenSlice (path, std::numeric_limits<std::size_t>::max (), 1) == AUDIO_SOURCE_RANGE_ERROR, "slice offset overflow rejected");
		Check (source.OpenSlice (path, 0, std::numeric_limits<std::size_t>::max ()) == AUDIO_SOURCE_RANGE_ERROR, "slice length overflow rejected");
	}

	void TestProbeContracts ()
	{
		std::vector<unsigned char> wave = MakeWave (1);
		AudioMemorySource source (&wave[0], wave.size ());
		AudioProbeResult result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_WAVE_PCM && result.PCM.SampleRate == 44100 && result.PCM.Channels == 2, "PCM WAVE probe");
		std::vector<unsigned char> floatWave = MakeWave (3);
		source.Assign (&floatWave[0], floatWave.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_WAVE_FLOAT && result.PCM.FloatingPoint, "float WAVE probe");
		Check (source.Tell () == 0, "probe restores initial position");
		Check (source.Seek (AUDIO_SEEK_BEGIN, 1) == AUDIO_SOURCE_OK, "position restoration setup");
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_UNKNOWN && source.Tell () == 1, "probe restores nonzero position");

		std::vector<unsigned char> vorbis = MakeOgg ("\x01vorbis", 7);
		source.Assign (&vorbis[0], vorbis.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_OGG_VORBIS, "Ogg Vorbis probe");
		std::vector<unsigned char> opus = MakeOgg ("OpusHead", 8);
		source.Assign (&opus[0], opus.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_OGG_OPUS, "Ogg Opus probe");
		const unsigned char flac[] = { 'f', 'L', 'a', 'C' };
		source.Assign (flac, sizeof (flac));
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_FLAC, "FLAC probe");
		std::vector<unsigned char> mpeg = MakeMpeg ();
		source.Assign (&mpeg[0], mpeg.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_MPEG, "MPEG probe");
		std::vector<unsigned char> mpegHeaderOnly (mpeg.begin (), mpeg.begin () + 4);
		source.Assign (&mpegHeaderOnly[0], mpegHeaderOnly.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "MPEG header without frame remains unknown");
		std::vector<unsigned char> truncatedMpeg (mpeg.begin (), mpeg.end () - 1);
		source.Assign (&truncatedMpeg[0], truncatedMpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "truncated MPEG frame remains unknown");
		std::vector<unsigned char> reservedMpeg (mpeg);
		reservedMpeg[3] = 2;
		source.Assign (&reservedMpeg[0], reservedMpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "reserved MPEG emphasis rejected");
		const unsigned char falseMpeg[] = { 0xff, 0xe0, 0, 0 };
		source.Assign (falseMpeg, sizeof (falseMpeg));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "false MPEG sync rejected");
		const unsigned char truncatedRiff[] = { 'R', 'I', 'F', 'F' };
		source.Assign (truncatedRiff, sizeof (truncatedRiff));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "truncated RIFF rejected");
		const unsigned char truncatedOgg[] = { 'O', 'g', 'g', 'S' };
		source.Assign (truncatedOgg, sizeof (truncatedOgg));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "truncated Ogg rejected");
		source.Assign (NULL, 0);
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "empty input remains unknown");

		AudioDecoder *decoder = NULL;
		Check (CreateAudioDecoder (source, result, &decoder) == AUDIO_DECODE_INVALID_SOURCE && decoder == NULL, "decoder factory rejects unknown probe");
		FailingSource readFailure (mpeg, true, false);
		Check (ProbeAudioFormat (readFailure).Status == AUDIO_PROBE_IO_ERROR && readFailure.Tell () == 0, "injected read failure restores source");
		FailingSource restoreFailure (mpeg, false, true);
		Check (ProbeAudioFormat (restoreFailure).Status == AUDIO_PROBE_IO_ERROR, "restore failure reported as I/O error");
	}

	void TestId3ProbeContracts ()
	{
		AudioMemorySource source;
		AudioProbeResult result;
		std::vector<unsigned char> id3Mpeg = MakeId3Mpeg ();
		source.Assign (&id3Mpeg[0], id3Mpeg.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_MPEG, "ID3 plus MPEG probe");
		std::vector<unsigned char> id3TruncatedMpeg (id3Mpeg.begin (), id3Mpeg.end () - 1);
		source.Assign (&id3TruncatedMpeg[0], id3TruncatedMpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "ID3 plus truncated MPEG remains unknown");
		std::vector<unsigned char> largeId3Mpeg = MakeId3Mpeg (8192, true);
		source.Assign (&largeId3Mpeg[0], largeId3Mpeg.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_MPEG && source.Tell () == 0, "large ID3 frame plus footer and MPEG probe restores position");
		std::vector<unsigned char> boundedId3Mpeg = MakeId3Mpeg (1024 * 1024, true);
		source.Assign (&boundedId3Mpeg[0], boundedId3Mpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_RECOGNIZED, "maximum ID3 tag with footer probe");
		std::vector<unsigned char> oversizedId3Mpeg = MakeId3Mpeg (1024 * 1024 + 1);
		source.Assign (&oversizedId3Mpeg[0], oversizedId3Mpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED && source.Tell () == 0, "oversized ID3 tag rejected");
		std::vector<unsigned char> truncatedLargeId3 = MakeId3Mpeg (8192);
		truncatedLargeId3.resize (10 + 8191);
		source.Assign (&truncatedLargeId3[0], truncatedLargeId3.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED && source.Tell () == 0, "truncated large ID3 rejected");
		const unsigned char id3Only[] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0 };
		source.Assign (id3Only, sizeof (id3Only));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "ID3 without MPEG remains unknown");
		const unsigned char invalidId3[] = { 'I', 'D', '3', 4, 0, 0, 0x80, 0, 0, 0 };
		source.Assign (invalidId3, sizeof (invalidId3));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "invalid ID3 rejected");
		std::vector<unsigned char> badRevision = MakeId3Mpeg ();
		badRevision[4] = 0xff;
		source.Assign (&badRevision[0], badRevision.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 bad revision plus MPEG rejected");
		std::vector<unsigned char> missingFooter = MakeId3Mpeg (11);
		missingFooter[5] = 0x10;
		source.Assign (&missingFooter[0], missingFooter.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 missing footer plus MPEG rejected");
		std::vector<unsigned char> badFooter = MakeId3Mpeg (11, true);
		badFooter[21] = 'X';
		source.Assign (&badFooter[0], badFooter.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 bad footer identifier rejected");
		std::vector<unsigned char> mismatchedFooter = MakeId3Mpeg (11, true);
		mismatchedFooter[26] = 0;
		source.Assign (&mismatchedFooter[0], mismatchedFooter.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 mismatched footer flags rejected");
		std::vector<unsigned char> mismatchedFooterSize = MakeId3Mpeg (11, true);
		mismatchedFooterSize[29] = 12;
		source.Assign (&mismatchedFooterSize[0], mismatchedFooterSize.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 mismatched footer size rejected");
		std::vector<unsigned char> prefixedId3Mpeg (1, 0);
		prefixedId3Mpeg.insert (prefixedId3Mpeg.end (), largeId3Mpeg.begin (), largeId3Mpeg.end ());
		FailingSource extendedReadFailure (prefixedId3Mpeg, false, false, 2);
		Check (extendedReadFailure.Seek (AUDIO_SEEK_BEGIN, 1) == AUDIO_SOURCE_OK, "extended read nonzero position setup");
		Check (ProbeAudioFormat (extendedReadFailure).Status == AUDIO_PROBE_IO_ERROR && extendedReadFailure.Tell () == 1, "extended ID3 read failure restores nonzero position");
		const unsigned char truncatedId3[] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 1 };
		source.Assign (truncatedId3, sizeof (truncatedId3));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "truncated ID3 rejected");
	}

	void TestDecoderContract ()
	{
		ContractDecoder decoder;
		AudioFrameRange loop;
		unsigned long long totalFrames = 0;
		short frames[2] = { 0, 0 };
		std::size_t framesRead = 0;
		Check (decoder.GetEncodedFormat () == AUDIO_FORMAT_MPEG && decoder.GetNativeSampleRate () == 48000 && decoder.GetOutputChannels () == 2, "decoder encoded and native format contract");
		Check (decoder.GetTotalFrames (&totalFrames) && totalFrames == 1 && decoder.GetLoopRange (&loop) && loop.Start == 0 && loop.End == 1, "decoder optional frame contracts");
		Check (decoder.GetDiagnosticCode () == 17 && strcmp (decoder.GetDiagnosticString (), "test diagnostic") == 0, "decoder diagnostic contract");
		Check (decoder.ReadFrames (frames, 2, &framesRead) == AUDIO_DECODER_DATA && framesRead == 1, "decoder partial final read contract");
		Check (decoder.ReadFrames (frames, 2, &framesRead) == AUDIO_DECODER_EOF && framesRead == 0, "decoder clean EOF contract");
		Check (decoder.SeekFrame (1) == AUDIO_DECODER_SEEK_OK && decoder.TellFrame () == 1, "decoder seek contract");
		Check (decoder.SeekFrame (2) == AUDIO_DECODER_SEEK_ERROR && decoder.TellFrame () == 1, "decoder failed seek preserves position");
	}

	void TestWavDecoder ()
	{
		std::vector<unsigned char> wave = MakeDecodableWave (2);
		AudioMemorySource source (&wave[0], wave.size ());
		AudioProbeResult probe = ProbeAudioFormat (source);
		AudioDecoder *decoder = NULL;
		short frames[8] = { 0 };
		std::size_t framesRead = 0;
		Check (CreateAudioDecoder (source, probe, &decoder) == AUDIO_DECODE_OK && decoder != NULL, "PCM WAVE decoder creates");
		if (decoder != NULL)
		{
			Check (decoder->GetNativeSampleRate () == 8000 && decoder->GetOutputChannels () == 2, "PCM WAVE decoder properties");
			Check (decoder->ReadFrames (frames, 4, &framesRead) == AUDIO_DECODER_DATA && framesRead == 3 && frames[0] == 100 && frames[5] == -300, "PCM WAVE partial final read");
			Check (decoder->ReadFrames (frames, 1, &framesRead) == AUDIO_DECODER_EOF && framesRead == 0, "PCM WAVE EOF");
			Check (decoder->SeekFrame (1) == AUDIO_DECODER_SEEK_OK && decoder->TellFrame () == 1, "PCM WAVE seek");
			Check (decoder->ReadFrames (frames, 1, &framesRead) == AUDIO_DECODER_DATA && framesRead == 1 && frames[0] == 200, "PCM WAVE seeked PCM");
			delete decoder;
		}
		wave = MakeDecodableWave (3);
		source.Assign (&wave[0], wave.size ());
		probe = ProbeAudioFormat (source);
		decoder = NULL;
		Check (CreateAudioDecoder (source, probe, &decoder) == AUDIO_DECODE_INVALID_SOURCE && decoder == NULL, "multichannel PCM WAVE rejected");
		wave = MakeDecodableWave (2);
		source.Assign (&wave[0], wave.size ());
		probe = ProbeAudioFormat (source);
		probe.Format = AUDIO_FORMAT_FLAC;
		decoder = NULL;
		Check (CreateAudioDecoder (source, probe, &decoder) == AUDIO_DECODE_INVALID_SOURCE && decoder == NULL, "WAVE input cannot fall back from FLAC probe");
	}

	template<typename T> const unsigned char *MakeUnalignedSamples (const T *samples, std::size_t sampleCount, std::vector<unsigned char> *bytes)
	{
		bytes->assign (sampleCount * sizeof (*samples) + 1, 0);
		memcpy (&(*bytes)[1], samples, sampleCount * sizeof (*samples));
		return &(*bytes)[1];
	}

	void TestMiniaudioPCM16Conversion ()
	{
		const short pcm16[] = { -32768, -1, 0, 32767 };
		std::vector<unsigned char> bytes;
		short s16[6] = { 123, 123, 123, 123, 123, 123 };
		float f32[6] = { 123.0f, 123.0f, 123.0f, 123.0f, 123.0f, 123.0f };
		int s32[6] = { 123, 123, 123, 123, 123, 123 };
		const unsigned char *input = MakeUnalignedSamples (pcm16, sizeof (pcm16) / sizeof (pcm16[0]), &bytes);
		AudioDecoderTestMiniaudioPCMToS16 (s16, input, 4, sizeof (pcm16[0]));
		AudioDecoderTestMiniaudioPCMToF32 (f32, input, 4, sizeof (pcm16[0]));
		AudioDecoderTestMiniaudioPCMToS32 (s32, input, 4, sizeof (pcm16[0]));
		Check (s16[0] == -32768 && s16[1] == -1 && s16[2] == 0 && s16[3] == 32767, "miniaudio PCM16 unaligned conversion to s16");
		Check (f32[0] == -1.0f && f32[1] == -0.000030517578125f && f32[2] == 0.0f && f32[3] == 0.999969482421875f, "miniaudio PCM16 unaligned conversion to f32");
		Check (s32[0] == INT_MIN && s32[1] == -65536 && s32[2] == 0 && s32[3] == 2147418112, "miniaudio PCM16 unaligned conversion to s32");
	}

	void TestMiniaudioPCM32Conversion ()
	{
		const int pcm32[] = { INT_MIN, -65536, 0, INT_MAX };
		std::vector<unsigned char> bytes;
		short s16[6] = { 123, 123, 123, 123, 123, 123 };
		float f32[6] = { 123.0f, 123.0f, 123.0f, 123.0f, 123.0f, 123.0f };
		int s32[6] = { 123, 123, 123, 123, 123, 123 };
		const unsigned char *input = MakeUnalignedSamples (pcm32, sizeof (pcm32) / sizeof (pcm32[0]), &bytes);
		AudioDecoderTestMiniaudioPCMToS16 (s16, input, 4, sizeof (pcm32[0]));
		AudioDecoderTestMiniaudioPCMToF32 (f32, input, 4, sizeof (pcm32[0]));
		AudioDecoderTestMiniaudioPCMToS32 (s32, input, 4, sizeof (pcm32[0]));
		Check (s16[0] == -32768 && s16[1] == -1 && s16[2] == 0 && s16[3] == 32767, "miniaudio PCM32 unaligned conversion and rounding to s16");
		Check (f32[0] == -1.0f && f32[1] == -0.000030517578125f && f32[2] == 0.0f && f32[3] > 0.9999f, "miniaudio PCM32 unaligned conversion to f32");
		Check (s32[0] == INT_MIN && s32[1] == -65536 && s32[2] == 0 && s32[3] == INT_MAX, "miniaudio PCM32 unaligned conversion to s32");
	}

	void TestMiniaudioIEEE32Conversion ()
	{
		const float float32[] = { -2.0f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f };
		std::vector<unsigned char> bytes;
		short s16[6] = { 123, 123, 123, 123, 123, 123 };
		float f32[6] = { 123.0f, 123.0f, 123.0f, 123.0f, 123.0f, 123.0f };
		int s32[6] = { 123, 123, 123, 123, 123, 123 };
		const unsigned char *input = MakeUnalignedSamples (float32, sizeof (float32) / sizeof (float32[0]), &bytes);
		AudioDecoderTestMiniaudioIEEEToS16 (s16, input, 6, sizeof (float32[0]));
		AudioDecoderTestMiniaudioIEEEToF32 (f32, input, 6, sizeof (float32[0]));
		AudioDecoderTestMiniaudioIEEEToS32 (s32, input, 6, sizeof (float32[0]));
		Check (s16[0] == -32768 && s16[1] == -32768 && s16[2] == -1 && s16[3] == 16383 && s16[4] == 32767 && s16[5] == 32767, "miniaudio IEEE float32 unaligned clamp and rounding to s16");
		Check (f32[0] == -2.0f && f32[1] == -1.0f && f32[2] == 0.0f && f32[3] == 0.5f && f32[4] == 1.0f && f32[5] == 2.0f, "miniaudio IEEE float32 unaligned conversion to f32");
		Check (s32[0] == INT_MIN && s32[1] == INT_MIN && s32[2] == 0 && s32[3] == 1073741824 && s32[4] == INT_MIN && s32[5] == INT_MIN, "miniaudio IEEE float32 unaligned clamp and rounding to s32");
	}

	void TestMiniaudioIEEE64Conversion ()
	{
		const short pcm16[] = { -32768, -1, 0, 32767 };
		const double float64[] = { -2.0, -1.0, 0.0, 0.5, 1.0, 2.0 };
		std::vector<unsigned char> bytes;
		short s16[6] = { 123, 123, 123, 123, 123, 123 };
		float f32[6] = { 123.0f, 123.0f, 123.0f, 123.0f, 123.0f, 123.0f };
		int s32[6] = { 123, 123, 123, 123, 123, 123 };
		const unsigned char *input = MakeUnalignedSamples (float64, sizeof (float64) / sizeof (float64[0]), &bytes);
		AudioDecoderTestMiniaudioIEEEToS16 (s16, input, 6, sizeof (float64[0]));
		AudioDecoderTestMiniaudioIEEEToF32 (f32, input, 6, sizeof (float64[0]));
		AudioDecoderTestMiniaudioIEEEToS32 (s32, input, 6, sizeof (float64[0]));
		Check (s16[0] == -32768 && s16[1] == -32768 && s16[2] == -1 && s16[3] == 16383 && s16[4] == 32767 && s16[5] == 32767, "miniaudio IEEE float64 unaligned clamp and rounding to s16");
		Check (f32[0] == -2.0f && f32[1] == -1.0f && f32[2] == 0.0f && f32[3] == 0.5f && f32[4] == 1.0f && f32[5] == 2.0f, "miniaudio IEEE float64 unaligned conversion to f32");
		Check (s32[0] == INT_MIN && s32[1] == INT_MIN && s32[2] == 0 && s32[3] == 1073741824 && s32[4] == INT_MIN && s32[5] == INT_MIN, "miniaudio IEEE float64 unaligned clamp and rounding to s32");
		AudioDecoderTestMiniaudioPCMToS16 (s16, input, 0, sizeof (pcm16[0]));
		AudioDecoderTestMiniaudioPCMToF32 (f32, input, 0, sizeof (pcm16[0]));
		AudioDecoderTestMiniaudioPCMToS32 (s32, input, 0, sizeof (pcm16[0]));
		AudioDecoderTestMiniaudioIEEEToS16 (s16, input, 0, sizeof (float64[0]));
		AudioDecoderTestMiniaudioIEEEToF32 (f32, input, 0, sizeof (float64[0]));
		AudioDecoderTestMiniaudioIEEEToS32 (s32, input, 0, sizeof (float64[0]));
		Check (s16[0] == -32768 && f32[0] == -2.0f && s32[0] == INT_MIN, "miniaudio zero-sample conversion preserves output");
	}

	void TestMiniaudioWavConversionHelpers ()
	{
		TestMiniaudioPCM16Conversion ();
		TestMiniaudioPCM32Conversion ();
		TestMiniaudioIEEE32Conversion ();
		TestMiniaudioIEEE64Conversion ();
	}

	void TestMiniaudioWavReaderBoundaries ()
	{
		const std::size_t pcm16Sizes[] = { 4095, 4096, 4097 };
		const std::size_t pcm32Sizes[] = { 4093, 4094, 4095, 4096, 4097 };
		const std::size_t ieee64Sizes[] = { 4089, 4090, 4091, 4092, 4093, 4094, 4095, 4096, 4097 };
		const std::size_t *sizes[] = { pcm16Sizes, pcm32Sizes, ieee64Sizes };
		const std::size_t counts[] = { sizeof (pcm16Sizes) / sizeof (pcm16Sizes[0]), sizeof (pcm32Sizes) / sizeof (pcm32Sizes[0]), sizeof (ieee64Sizes) / sizeof (ieee64Sizes[0]) };
		const unsigned int formats[] = { 1, 1, 3 };
		const unsigned int widths[] = { 16, 32, 64 };
		const unsigned long long samples[] = { 0x8000, 0x80000000ULL, 0xbff0000000000000ULL };
		for (unsigned int formatIndex = 0; formatIndex < 3; ++formatIndex)
		{
			for (std::size_t sizeIndex = 0; sizeIndex < counts[formatIndex]; ++sizeIndex)
			{
				std::size_t dataBytes = sizes[formatIndex][sizeIndex];
				std::size_t bytesPerSample = widths[formatIndex] / 8;
				std::vector<unsigned char> wave = MakeReaderWave (formats[formatIndex], widths[formatIndex], samples[formatIndex], dataBytes);
				std::vector<short> output (dataBytes / bytesPerSample + 1, 12345);
				std::size_t framesRead = AudioDecoderTestMiniaudioReadWavS16 (&wave[0], wave.size (), &output[0], output.size ());
				Check (framesRead == dataBytes / bytesPerSample && output[framesRead] == 12345, "miniaudio WAV reader preserves partial sample and output canary");
				Check (framesRead == 0 || output[0] == -32768, "miniaudio WAV reader converts unaligned scalar sample");
			}
		}
	}

	void TestTemporaryFixtureCollisionExhaustion (const std::vector<unsigned char> &bytes)
	{
		unsigned int sequence = 0;
		std::string createdPath;
		std::vector<std::string> collisionPaths;
		for (unsigned int collision = 0; collision < 64; ++collision)
		{
			char name[128];
			sprintf (name, "audio_decoder_fallback_%lu_%u_exhaustion.wav", FixtureProcessId (), collision + 1);
			int fileDescriptor = OpenFixtureFile (name);
			if (fileDescriptor < 0)
			{
				break;
			}
			if (!WriteFixtureBytes (fileDescriptor, bytes))
			{
				CloseFixtureFile (fileDescriptor);
				remove (name);
				break;
			}
			CloseFixtureFile (fileDescriptor);
			collisionPaths.push_back (name);
		}
		if (collisionPaths.size () != 64)
		{
			for (std::vector<std::string>::const_iterator path = collisionPaths.begin (); path != collisionPaths.end (); ++path)
			{
				remove (path->c_str ());
			}
			return;
		}
		Check (!WriteUniqueTemporaryFixture (bytes, "exhaustion", NULL, &sequence, &createdPath), "temporary fixture stops after 64 name collisions");
		for (std::vector<std::string>::const_iterator path = collisionPaths.begin (); path != collisionPaths.end (); ++path)
		{
			std::vector<unsigned char> marker;
			Check (ReadFileBytes (path->c_str (), &marker) && marker == bytes, "temporary fixture collision marker remains unchanged");
			remove (path->c_str ());
		}
	}

	void TestTemporaryFixtureCreationFailures ()
	{
		try
		{
			const unsigned char data[] = { 0, 1, 2, 3 };
			const std::vector<unsigned char> bytes (data, data + sizeof (data));
			unsigned int sequence = 0;
			char collisionPath[128];
			bool collisionPathOwned;
			bool collisionReady;
			std::string createdPath;
			std::vector<unsigned char> initialMarker;
			sprintf (collisionPath, "audio_decoder_fallback_%lu_%u_collision.wav", FixtureProcessId (), sequence + 1);
			int fileDescriptor = OpenFixtureFile (collisionPath);
			collisionPathOwned = fileDescriptor >= 0;
			collisionReady = false;
			if (collisionPathOwned)
			{
				collisionReady = WriteFixtureBytes (fileDescriptor, bytes);
				CloseFixtureFile (fileDescriptor);
			}
			if (collisionReady)
			{
				bool fixtureCreated = WriteUniqueTemporaryFixture (bytes, "collision", NULL, &sequence, &createdPath);
				Check (fixtureCreated && createdPath != collisionPath, "temporary fixture skips a known name collision");
				Check (ReadFileBytes (collisionPath, &initialMarker) && initialMarker == bytes, "initial collision fixture remains unchanged");
				if (fixtureCreated)
				{
					remove (createdPath.c_str ());
				}
			}
			if (collisionPathOwned)
			{
				remove (collisionPath);
			}
			sequence = 0;
			Check (!WriteUniqueTemporaryFixture (bytes, "invalid", "audio_decoder_tests_missing_fixture_directory", &sequence, &createdPath), "temporary fixture stops after a non-collision create failure");
			TestTemporaryFixtureCollisionExhaustion (bytes);
		}
		catch (...)
		{
			Check (false, "temporary fixture failure test setup");
		}
	}

	void TestMiniaudioAutoDetectionFallback ()
	{
		std::vector<unsigned char> bytes;
		TemporaryFixtureFiles temporaryFixtures;
		std::string flacPath = FixturePath ("flac_mono.flac");
		std::string mp3Path = FixturePath ("mp3_mono.mp3");
		std::string fallbackPath;
		std::wstring wideFallbackPath;
		const unsigned char invalid[] = { 0, 1, 2, 3 };
		Check (ReadFileBytes (flacPath.c_str (), &bytes) && AudioDecoderTestMiniaudioAutoDetectMemory (&bytes[0], bytes.size ()), "miniaudio WAV failure falls back to FLAC from memory");
		Check (ReadFileBytes (mp3Path.c_str (), &bytes) && AudioDecoderTestMiniaudioAutoDetectMemory (&bytes[0], bytes.size ()), "miniaudio WAV and FLAC failures fall back to MP3 from memory");
		Check (ReadFileBytes (flacPath.c_str (), &bytes) && temporaryFixtures.Write (bytes, "flac", &fallbackPath) && AudioDecoderTestMiniaudioAutoDetectFile (fallbackPath.c_str ()), "miniaudio WAV-first file fallback opens FLAC fixture");
		wideFallbackPath.assign (fallbackPath.begin (), fallbackPath.end ());
		Check (!wideFallbackPath.empty () && AudioDecoderTestMiniaudioAutoDetectWideFile (wideFallbackPath.c_str ()), "miniaudio WAV-first wide-file fallback opens FLAC fixture");
		Check (ReadFileBytes (mp3Path.c_str (), &bytes) && temporaryFixtures.Write (bytes, "mp3", &fallbackPath) && AudioDecoderTestMiniaudioAutoDetectFile (fallbackPath.c_str ()), "miniaudio WAV-first file fallback opens MP3 fixture");
		wideFallbackPath.assign (fallbackPath.begin (), fallbackPath.end ());
		Check (!wideFallbackPath.empty () && AudioDecoderTestMiniaudioAutoDetectWideFile (wideFallbackPath.c_str ()), "miniaudio WAV-first wide-file fallback opens MP3 fixture");
		Check (temporaryFixtures.Write (std::vector<unsigned char> (invalid, invalid + sizeof (invalid)), "invalid", &fallbackPath) && !AudioDecoderTestMiniaudioAutoDetectFile (fallbackPath.c_str ()) && !AudioDecoderTestMiniaudioAutoDetectWideFile (std::wstring (fallbackPath.begin (), fallbackPath.end ()).c_str ()), "miniaudio WAV-first file and wide-file reject input unsupported by every fallback decoder");
		Check (!AudioDecoderTestMiniaudioAutoDetectMemory (invalid, sizeof (invalid)), "miniaudio rejects input unsupported by every fallback decoder");
	}

	void TestCodecFixtures ()
	{
		TestFixtureParity ("pcm16_stereo.wav", AUDIO_FORMAT_WAVE_PCM, 8000, 2, 529, 0xbd9ea28d7d1f5ebeULL, "PCM WAVE fixture parity");
		TestFixtureParity ("float32_mono.wav", AUDIO_FORMAT_WAVE_FLOAT, 8000, 1, 529, 0x00149f86578b3dbaULL, "float WAVE fixture parity");
		TestFixtureParity ("flac_mono.flac", AUDIO_FORMAT_FLAC, 8000, 1, 529, 0xae1a6ff7e4b9ff1bULL, "mono FLAC fixture parity");
		TestFixtureParity ("flac_stereo.flac", AUDIO_FORMAT_FLAC, 8000, 2, 529, 0xbd9ea28d7d1f5ebeULL, "stereo FLAC fixture parity");
		TestFixtureParity ("mp3_mono.mp3", AUDIO_FORMAT_MPEG, 8000, 1, 1728, 0xc0324ec2431f1394ULL, "MP3 fixture parity");
		TestFixtureParity ("vorbis_mono.ogg", AUDIO_FORMAT_OGG_VORBIS, 8000, 1, 529, 0x04b79fb4e2b4a807ULL, "mono Ogg Vorbis fixture parity");
		TestFixtureParity ("vorbis_stereo.ogg", AUDIO_FORMAT_OGG_VORBIS, 8000, 2, 529, 0xf7005ad1fbefb6c2ULL, "stereo Ogg Vorbis fixture parity");
		TestTruncatedFixture ("flac_mono.flac", "truncated FLAC rejected");
		TestTruncatedFixture ("mp3_mono.mp3", "truncated MP3 rejected");
		TestTruncatedFixture ("vorbis_mono.ogg", "truncated Ogg Vorbis rejected");
		{
			std::string path = FixturePath ("pcm16_three_channel.wav");
			AudioFileSource source;
			AudioDecoder *decoder = NULL;
			Check (source.Open (path.c_str ()) == AUDIO_SOURCE_OK, "multichannel WAVE fixture opens");
			Check (CreateAudioDecoder (source, ProbeAudioFormat (source), &decoder) == AUDIO_DECODE_INVALID_SOURCE && decoder == NULL, "multichannel WAVE fixture rejected");
		}
		{
			std::string path = FixturePath ("vorbis_three_channel.ogg");
			AudioFileSource source;
			AudioDecoder *decoder = NULL;
			Check (source.Open (path.c_str ()) == AUDIO_SOURCE_OK, "multichannel Ogg Vorbis fixture opens");
			Check (CreateAudioDecoder (source, ProbeAudioFormat (source), &decoder) == AUDIO_DECODE_INVALID_SOURCE && decoder == NULL, "multichannel Ogg Vorbis fixture rejected");
		}
		{
			std::vector<unsigned char> opus = MakeOgg ("OpusHead", 8);
			AudioMemorySource source (&opus[0], opus.size ());
			AudioDecoder *decoder = NULL;
			Check (CreateAudioDecoder (source, ProbeAudioFormat (source), &decoder) == AUDIO_DECODE_UNSUPPORTED && decoder == NULL, "Ogg Opus explicitly unsupported");
		}
		const unsigned int maxVorbisFrames = 0xfffffffdU;
		Check (!AudioDecoderTestSupportsVorbisChannels (0) && AudioDecoderTestSupportsVorbisChannels (1) && AudioDecoderTestSupportsVorbisChannels (2) && !AudioDecoderTestSupportsVorbisChannels (3), "Vorbis channel cap seam");
		Check (AudioDecoderTestSupportsVorbisTotalFrames (maxVorbisFrames - 1) && AudioDecoderTestSupportsVorbisTotalFrames (maxVorbisFrames) && !AudioDecoderTestSupportsVorbisTotalFrames (0xfffffffeU) && !AudioDecoderTestSupportsVorbisTotalFrames (0xffffffffU), "Vorbis total-frame cap boundaries");
		Check (AudioDecoderTestVorbisCanReadFrames (maxVorbisFrames, maxVorbisFrames - 1, 1) && !AudioDecoderTestVorbisCanReadFrames (maxVorbisFrames, maxVorbisFrames, 1) && AudioDecoderTestVorbisCanReadFrames (maxVorbisFrames, maxVorbisFrames, 0), "Vorbis final-frame read boundary");
		Check (AudioDecoderTestVorbisCanSeekFrame (maxVorbisFrames, maxVorbisFrames - 1) && AudioDecoderTestVorbisCanSeekFrame (maxVorbisFrames, maxVorbisFrames) && AudioDecoderTestVorbisIsLogicalEOF (maxVorbisFrames, maxVorbisFrames), "Vorbis exact logical EOF seek");
		Check (!AudioDecoderTestVorbisCanSeekFrame (maxVorbisFrames, 0xfffffffeU) && !AudioDecoderTestVorbisCanSeekFrame (maxVorbisFrames, 0xffffffffU) && !AudioDecoderTestVorbisCanUseLoopEndpoint (maxVorbisFrames, 0xfffffffeU) && !AudioDecoderTestVorbisCanUseLoopEndpoint (maxVorbisFrames, 0xffffffffU), "Vorbis reserved sentinel seek and loop endpoints rejected");
	}

	void TestMiniaudioFlacAllocationBounds ()
	{
		std::size_t allocationSize = 0;
		std::size_t decodedSamplesOffset = 0;
		std::size_t decodedSampleCount = 0;
		std::size_t seekpointsOffset = 0;
		Check (AudioDecoderTestMiniaudioFlacAllocationLayout (4096, 2, 3, false, &allocationSize, &decodedSamplesOffset, &decodedSampleCount, &seekpointsOffset), "miniaudio FLAC allocation layout accepts stereo stream");
		Check (allocationSize >= decodedSamplesOffset + decodedSampleCount * sizeof (int) && seekpointsOffset >= decodedSamplesOffset + decodedSampleCount * sizeof (int) && allocationSize >= seekpointsOffset + 3 * 18, "miniaudio FLAC allocation layout reserves SIMD alignment margin");
		Check (!AudioDecoderTestMiniaudioFlacAllocationLayout (UINT_MAX, UINT_MAX, UINT_MAX, false, &allocationSize, &decodedSamplesOffset, &decodedSampleCount, &seekpointsOffset), "miniaudio FLAC allocation layout rejects overflow");
		Check (AudioDecoderTestMiniaudioFlacAllocationLayout (65535, 8, 932067, false, &allocationSize, &decodedSamplesOffset, &decodedSampleCount, &seekpointsOffset) && decodedSampleCount == (std::size_t)65536 * 8 && seekpointsOffset >= decodedSamplesOffset + decodedSampleCount * sizeof (int) && allocationSize >= seekpointsOffset + (std::size_t)932067 * 18, "miniaudio native FLAC maximum layout rounds block size and retains seekpoints");
		Check (AudioDecoderTestMiniaudioFlacAllocationLayout (65535, 8, 932067, true, &allocationSize, &decodedSamplesOffset, &decodedSampleCount, &seekpointsOffset) && decodedSampleCount == (std::size_t)65536 * 8 && seekpointsOffset >= decodedSamplesOffset + decodedSampleCount * sizeof (int) && allocationSize >= seekpointsOffset + (std::size_t)932067 * 18, "miniaudio Ogg-FLAC maximum layout rounds block size and retains seekpoints");
	}

	void TestMiniaudioFlacAllocationOwnership (std::vector<unsigned char> &flac)
	{
		std::size_t allocationCount = 0;
		std::size_t freeCount = 0;
		Check (ReadFileBytes (FixturePath ("flac_mono.flac").c_str (), &flac) && AudioDecoderTestMiniaudioFlacAllocationOwnership (&flac[0], flac.size (), &allocationCount, &freeCount), "miniaudio FLAC preserves callback parent allocation ownership");
		Check (allocationCount == 1 && freeCount == 1, "miniaudio FLAC releases its parent allocation once");
	}

	void TestMiniaudioFlacNativeFixtureLayout (const std::vector<unsigned char> &flac)
	{
		unsigned long long firstPCMFrame = 0;
		unsigned long long flacFrameOffset = 0;
		unsigned int pcmFrameCount = 0;
		unsigned int nativePCMFrameCount = 0;
		unsigned int metadataRawDataSize = 0;
		std::size_t nativeAudioOffset = 0;
		std::vector<FlacMetadataBlock> nativeMetadata;
		Check (!flac.empty () && AudioDecoderTestMiniaudioFlacOpenSeektable (&flac[0], flac.size (), false, &pcmFrameCount, &firstPCMFrame, &flacFrameOffset, &nativePCMFrameCount, &metadataRawDataSize) && pcmFrameCount == 0, "miniaudio native FLAC opens without a seektable");
		Check (ParseNativeFlacMetadata (flac, &nativeMetadata, &nativeAudioOffset) && nativeMetadata.size () == 3 && nativeMetadata[0].Offset == 4 && nativeMetadata[0].Bytes == 38 && nativeMetadata[0].Type == 0 && !nativeMetadata[0].IsLast && nativeMetadata[1].Offset == 42 && nativeMetadata[1].Bytes == 18 && nativeMetadata[1].Type == 4 && !nativeMetadata[1].IsLast && nativeMetadata[2].Offset == 60 && nativeMetadata[2].Bytes == 8196 && nativeMetadata[2].Type == 1 && nativeMetadata[2].IsLast && nativeAudioOffset == 8256 && (((unsigned int)flac[10] << 8) | flac[11]) == 512, "native FLAC metadata walk finds comment, final padding, and audio frame offset");
	}

	void TestMiniaudioFlacValidSeektableOpen (const std::vector<unsigned char> &flac, std::vector<unsigned char> &seektableFlac);
	void TestMiniaudioFlacValidSeektableMetadata (const std::vector<unsigned char> &seektableFlac);
	void TestMiniaudioFlacValidSeektableDecode (const std::vector<unsigned char> &seektableFlac, AudioDecodedPCM16 &nativePCM);
	void TestMiniaudioOggFlacSeektableDecode (const std::vector<unsigned char> &seektableFlac, std::vector<unsigned char> &oggFlac, const AudioDecodedPCM16 &nativePCM);
	void TestMiniaudioOggFlacSeektableTemporaryOOM (const std::vector<unsigned char> &oggFlac);
	void TestMiniaudioOggFlacSeektableParentOOM (const std::vector<unsigned char> &oggFlac);

	void TestMiniaudioFlacValidSeektable (const std::vector<unsigned char> &flac, std::vector<unsigned char> &seektableFlac, AudioDecodedPCM16 &nativePCM)
	{
		TestMiniaudioFlacValidSeektableOpen (flac, seektableFlac);
		TestMiniaudioFlacValidSeektableMetadata (seektableFlac);
		TestMiniaudioFlacValidSeektableDecode (seektableFlac, nativePCM);
	}

	void TestMiniaudioFlacValidSeektableOpen (const std::vector<unsigned char> &flac, std::vector<unsigned char> &seektableFlac)
	{
		unsigned long long firstPCMFrame = 0;
		unsigned long long flacFrameOffset = 0;
		unsigned int pcmFrameCount = 0;
		unsigned int nativePCMFrameCount = 0;
		unsigned int metadataRawDataSize = 0;
		Check (MakeFlacWithSeektable (flac, &seektableFlac) && AudioDecoderTestMiniaudioFlacOpenSeektable (&seektableFlac[0], seektableFlac.size (), false, &pcmFrameCount, &firstPCMFrame, &flacFrameOffset, &nativePCMFrameCount, &metadataRawDataSize) && pcmFrameCount == 1 && firstPCMFrame == 0 && flacFrameOffset == 0 && nativePCMFrameCount == 512, "miniaudio native FLAC opens valid seektable at frame zero");
	}

	void TestMiniaudioFlacValidSeektableMetadata (const std::vector<unsigned char> &seektableFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), true, false, 0, (std::size_t)-1, 0, (std::size_t)-1, &callbacks) && callbacks.Opened && callbacks.MetadataSeektableCount == 1 && callbacks.MetadataRawDataMatchesSeekpoints && callbacks.MetadataRawDataSize == 18 && callbacks.MetadataSeekpointCount == 1 && callbacks.MetadataFirstPCMFrame == 0 && callbacks.MetadataFlacFrameOffset == 0 && callbacks.MetadataPCMFrameCount == 512, "miniaudio native FLAC metadata callback reports valid seektable");
	}

	void TestMiniaudioFlacValidSeektableDecode (const std::vector<unsigned char> &seektableFlac, AudioDecodedPCM16 &nativePCM)
	{
		{
			AudioMemorySource source (&seektableFlac[0], seektableFlac.size ());
			AudioProbeResult probe = ProbeAudioFormat (source);
			AudioDecoder *decoder = NULL;
			std::size_t framesRead = 0;
			short seekedSample = 0;
			Check (probe.Status == AUDIO_PROBE_RECOGNIZED && probe.Format == AUDIO_FORMAT_FLAC && DecodeAudioToPCM16 (source, probe, &nativePCM) == AUDIO_DECODE_OK && nativePCM.Samples.size () == 529 && HashPCM16 (nativePCM) == 0xae1a6ff7e4b9ff1bULL, "miniaudio native FLAC seektable decodes full fixture PCM");
			source.Assign (&seektableFlac[0], seektableFlac.size ());
			Check (CreateAudioDecoder (source, ProbeAudioFormat (source), &decoder) == AUDIO_DECODE_OK && decoder != NULL && decoder->SeekFrame (264) == AUDIO_DECODER_SEEK_OK && decoder->ReadFrames (&seekedSample, 1, &framesRead) == AUDIO_DECODER_DATA && framesRead == 1 && nativePCM.Samples.size () > 264 && seekedSample == nativePCM.Samples[264], "miniaudio native FLAC seektable seek reads reference sample");
			delete decoder;
		}
	}

	void TestMiniaudioOggFlacSeektable (const std::vector<unsigned char> &seektableFlac, const AudioDecodedPCM16 &nativePCM)
	{
		std::vector<unsigned char> oggFlac;
		TestMiniaudioOggFlacSeektableDecode (seektableFlac, oggFlac, nativePCM);
		TestMiniaudioOggFlacSeektableTemporaryOOM (oggFlac);
		TestMiniaudioOggFlacSeektableParentOOM (oggFlac);
	}

	void TestMiniaudioOggFlacSeektableDecode (const std::vector<unsigned char> &seektableFlac, std::vector<unsigned char> &oggFlac, const AudioDecodedPCM16 &nativePCM)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		unsigned long long totalPCMFrames = 0;
		unsigned long long pcmHash = 0;
		short firstSample = 0;
		short seekSample = 0;
		bool prerequisites = MakeOggFlac (seektableFlac, 529, &oggFlac) && ValidateOggFlac (oggFlac, seektableFlac, 529);
		if (!prerequisites)
		{
			Check (false, "miniaudio Ogg-FLAC decodes valid packets, seeks, and reaches EOF through the low-level memory path");
			return;
		}
		bool decodeResult = AudioDecoderTestMiniaudioOggFlacDecode (&oggFlac[0], oggFlac.size (), 264, 0, &totalPCMFrames, &pcmHash, &firstSample, &seekSample, &callbacks);
		if (!decodeResult)
		{
			Check (false, "miniaudio Ogg-FLAC decodes valid packets, seeks, and reaches EOF through the low-level memory path");
			return;
		}
		Check (callbacks.Opened && totalPCMFrames == 529 && pcmHash == 0xae1a6ff7e4b9ff1bULL && nativePCM.Samples.size () > 264 && firstSample == nativePCM.Samples[0] && seekSample == nativePCM.Samples[264], "miniaudio Ogg-FLAC decodes valid packets, seeks, and reaches EOF through the low-level memory path");
		Check (callbacks.MallocCount == 2 && callbacks.ReallocCount == 0 && callbacks.FreeCount == 2 && callbacks.AllocationPointers[0] != callbacks.AllocationPointers[1] && callbacks.AllocationPointers[0] == callbacks.FreePointers[0] && callbacks.AllocationPointers[1] == callbacks.FreePointers[1] && callbacks.CanaryIntact, "miniaudio Ogg-FLAC transfers temporary Ogg state into one parent allocation");
	}

	void TestMiniaudioOggFlacSeektableTemporaryOOM (const std::vector<unsigned char> &oggFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		unsigned long long totalPCMFrames = 0;
		unsigned long long pcmHash = 0;
		short firstSample = 0;
		short seekSample = 0;
		Check (AudioDecoderTestMiniaudioOggFlacDecode (&oggFlac[0], oggFlac.size (), 0, 1, &totalPCMFrames, &pcmHash, &firstSample, &seekSample, &callbacks) && !callbacks.Opened && callbacks.MallocCount == 1 && callbacks.FreeCount == 0 && callbacks.CanaryIntact, "miniaudio Ogg-FLAC temporary allocation OOM returns null without free");
	}

	void TestMiniaudioOggFlacSeektableParentOOM (const std::vector<unsigned char> &oggFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		unsigned long long totalPCMFrames = 0;
		unsigned long long pcmHash = 0;
		short firstSample = 0;
		short seekSample = 0;
		Check (AudioDecoderTestMiniaudioOggFlacDecode (&oggFlac[0], oggFlac.size (), 0, 2, &totalPCMFrames, &pcmHash, &firstSample, &seekSample, &callbacks) && !callbacks.Opened && callbacks.MallocCount == 2 && callbacks.FreeCount == 1 && callbacks.AllocationPointers[0] == callbacks.FreePointers[0] && callbacks.CanaryIntact, "miniaudio Ogg-FLAC parent allocation OOM frees temporary state once");
	}

	void TestMiniaudioFlacSeekpointWire ()
	{
		const unsigned char seekpoint[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0x13, 0x57 };
		unsigned long long firstPCMFrame = 0;
		unsigned long long flacFrameOffset = 0;
		unsigned int pcmFrameCount = 0;
		Check (AudioDecoderTestMiniaudioFlacDecodeSeekpoint (seekpoint, sizeof (seekpoint), &firstPCMFrame, &flacFrameOffset, &pcmFrameCount) && firstPCMFrame == 0x0123456789abcdefULL && flacFrameOffset == 0x1032547698badcfeULL && pcmFrameCount == 0x1357, "miniaudio FLAC seekpoint wire decoding");
		Check (!AudioDecoderTestMiniaudioFlacDecodeSeekpoint (seekpoint, sizeof (seekpoint) - 1, &firstPCMFrame, &flacFrameOffset, &pcmFrameCount), "miniaudio FLAC seekpoint rejects short wire read");
	}

	void TestMiniaudioFlacCallbackWireScenarios (std::vector<unsigned char> &seektableFlac, std::vector<unsigned char> &shortSeektableFlac, unsigned long long *firstPCMFrame, unsigned long long *flacFrameOffset, unsigned int *pcmFrameCount, unsigned int *nativePCMFrameCount, unsigned int *metadataRawDataSize)
	{
		Check (AudioDecoderTestMiniaudioFlacOpenSeektable (&seektableFlac[0], seektableFlac.size (), false, pcmFrameCount, firstPCMFrame, flacFrameOffset, nativePCMFrameCount, metadataRawDataSize) && *pcmFrameCount == 1 && *firstPCMFrame == 0x0123456789abcdefULL && *flacFrameOffset == 0x1032547698badcfeULL && *nativePCMFrameCount == 0x1357, "miniaudio native FLAC open reads seektable wire fields");
		Check (AudioDecoderTestMiniaudioFlacOpenSeektable (&seektableFlac[0], seektableFlac.size (), true, pcmFrameCount, firstPCMFrame, flacFrameOffset, nativePCMFrameCount, metadataRawDataSize) && *pcmFrameCount == 1 && *metadataRawDataSize == 18, "miniaudio native FLAC metadata seektable retains wire size");
	}

	void TestMiniaudioFlacCallbackMetadataScenarios (const std::vector<unsigned char> &seektableFlac, const std::vector<unsigned char> &emptySeektableFlac, AudioDecoderTestMiniaudioFlacCallbackReport *callbacks)
	{
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), true, false, 0, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && callbacks->Opened && callbacks->MetadataSeektableCount == 1 && callbacks->MetadataRawDataMatchesSeekpoints && callbacks->MetadataRawDataSize == 18 && callbacks->MetadataSeekpointCount == 1 && callbacks->MetadataFirstPCMFrame == 0x0123456789abcdefULL && callbacks->MetadataFlacFrameOffset == 0x1032547698badcfeULL && callbacks->MetadataPCMFrameCount == 0x1357, "miniaudio native FLAC metadata callback exposes seekpoint array and wire size");
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&emptySeektableFlac[0], emptySeektableFlac.size (), true, false, 0, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && callbacks->Opened && callbacks->MetadataSeektableCount == 1 && callbacks->MetadataRawDataMatchesSeekpoints && callbacks->MetadataRawDataSize == 0 && callbacks->MetadataSeekpointCount == 0 && callbacks->MallocCount == 1 && callbacks->FreeCount == 1 && callbacks->CanaryIntact, "miniaudio native FLAC metadata callback exposes empty seektable without allocation");
	}

	void TestMiniaudioFlacCallbackAllocatorScenarios (const std::vector<unsigned char> &seektableFlac, AudioDecoderTestMiniaudioFlacCallbackReport *callbacks)
	{
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), false, false, 0, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && callbacks->Opened && callbacks->MallocCount == 1 && callbacks->ReallocCount == 0 && callbacks->FreeCount == 1 && callbacks->AllocationPointers[0] == callbacks->FreePointers[0] && callbacks->CanaryIntact, "miniaudio native FLAC malloc parent allocation is freed once");
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), false, false, 1, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && !callbacks->Opened && callbacks->MallocCount == 1 && callbacks->FreeCount == 0, "miniaudio native FLAC failed parent malloc returns null without free");
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), false, true, 0, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && callbacks->Opened && callbacks->MallocCount == 0 && callbacks->ReallocCount == 1 && callbacks->FreeCount == 1 && !callbacks->ReallocExistingPointer && callbacks->AllocationPointers[0] == callbacks->FreePointers[0] && callbacks->CanaryIntact, "miniaudio native FLAC realloc-only allocator routes null realloc and free");
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), false, true, 1, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && !callbacks->Opened && callbacks->ReallocCount == 1 && callbacks->FreeCount == 0, "miniaudio native FLAC failed realloc-only parent allocation returns null");
	}

	void TestMiniaudioFlacCallbackFailureScenarios (const std::vector<unsigned char> &seektableFlac, AudioDecoderTestMiniaudioFlacCallbackReport *callbacks)
	{
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), true, false, 0, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && callbacks->Opened && callbacks->MallocCount == 2 && callbacks->FreeCount == 2 && callbacks->AllocationPointers[0] == callbacks->FreePointers[0] && callbacks->AllocationPointers[1] == callbacks->FreePointers[1] && callbacks->CanaryIntact, "miniaudio native FLAC metadata temporary allocation and parent free sequentially");
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), true, false, 1, (std::size_t)-1, 0, (std::size_t)-1, callbacks) && !callbacks->Opened && callbacks->MallocCount == 1 && callbacks->FreeCount == 0, "miniaudio native FLAC metadata temporary allocation OOM returns null");
	}

	void TestMiniaudioFlacCallbackMetadataParentAllocationOOM (const std::vector<unsigned char> &seektableFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		bool callbackResult = AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), true, false, 2, (std::size_t)-1, 0, (std::size_t)-1, &callbacks);
		if (!callbackResult)
		{
			Check (false, "miniaudio native FLAC metadata callback completes before parent allocation OOM frees temporary state");
			return;
		}
		Check (!callbacks.Opened, "miniaudio native FLAC metadata callback completes before parent allocation OOM frees temporary state");
		Check (callbacks.MallocCount == 2, "miniaudio native FLAC metadata parent allocation OOM allocation count");
		Check (callbacks.ReallocCount == 0, "miniaudio native FLAC metadata parent allocation OOM realloc count");
		Check (callbacks.FreeCount == 1, "miniaudio native FLAC metadata parent allocation OOM free count");
		Check (callbacks.CanaryIntact, "miniaudio native FLAC metadata parent allocation OOM canary");
		Check (callbacks.MetadataSeektableCount == 1, "miniaudio native FLAC metadata parent allocation OOM seektable count");
		Check (callbacks.MetadataRawDataMatchesSeekpoints, "miniaudio native FLAC metadata parent allocation OOM raw data");
		Check (callbacks.MetadataRawDataSize == 18, "miniaudio native FLAC metadata parent allocation OOM raw data size");
		Check (callbacks.MetadataSeekpointCount == 1, "miniaudio native FLAC metadata parent allocation OOM seekpoint count");
		Check (callbacks.MetadataFirstPCMFrame == 0x0123456789abcdefULL, "miniaudio native FLAC metadata parent allocation OOM first PCM frame");
		Check (callbacks.MetadataFlacFrameOffset == 0x1032547698badcfeULL, "miniaudio native FLAC metadata parent allocation OOM FLAC frame offset");
		Check (callbacks.MetadataPCMFrameCount == 0x1357, "miniaudio native FLAC metadata parent allocation OOM PCM frame count");
		Check (callbacks.AllocationPointers[0] != NULL, "miniaudio native FLAC metadata parent allocation OOM temporary allocation");
		Check (callbacks.AllocationPointers[0] == callbacks.FreePointers[0], "miniaudio native FLAC metadata parent allocation OOM temporary free");
		Check (callbacks.AllocationPointers[1] == NULL, "miniaudio native FLAC metadata parent allocation OOM parent allocation");
	}

	void TestMiniaudioFlacCallbackMetadataShortRead (const std::vector<unsigned char> &seektableFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		bool callbackResult = AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), true, false, 0, 46, 0, (std::size_t)-1, &callbacks);
		if (!callbackResult)
		{
			Check (false, "miniaudio native FLAC metadata seektable short read frees temporary state before parent allocation");
			return;
		}
		Check (!callbacks.Opened, "miniaudio native FLAC metadata seektable short read frees temporary state before parent allocation");
		Check (callbacks.MallocCount == 1, "miniaudio native FLAC metadata short read allocation count");
		Check (callbacks.ReallocCount == 0, "miniaudio native FLAC metadata short read realloc count");
		Check (callbacks.FreeCount == 1, "miniaudio native FLAC metadata short read free count");
		Check (callbacks.CanaryIntact, "miniaudio native FLAC metadata short read canary");
		Check (callbacks.MetadataSeektableCount == 0, "miniaudio native FLAC metadata short read seektable count");
		Check (callbacks.MetadataRawDataSize == 0, "miniaudio native FLAC metadata short read raw data size");
		Check (callbacks.MetadataSeekpointCount == 0, "miniaudio native FLAC metadata short read seekpoint count");
		Check (callbacks.MetadataFirstPCMFrame == 0, "miniaudio native FLAC metadata short read first PCM frame");
		Check (callbacks.MetadataFlacFrameOffset == 0, "miniaudio native FLAC metadata short read FLAC frame offset");
		Check (callbacks.MetadataPCMFrameCount == 0, "miniaudio native FLAC metadata short read PCM frame count");
		Check (callbacks.AllocationPointers[0] != NULL, "miniaudio native FLAC metadata short read temporary allocation");
		Check (callbacks.AllocationPointers[0] == callbacks.FreePointers[0], "miniaudio native FLAC metadata short read temporary free");
		Check (callbacks.AllocationPointers[1] == NULL, "miniaudio native FLAC metadata short read parent allocation");
	}

	void TestMiniaudioFlacCallbackCanaryDamage (const std::vector<unsigned char> &seektableFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), true, false, 0, (std::size_t)-1, 0, (std::size_t)-1, &callbacks, true) && callbacks.Opened && !callbacks.CanaryIntact && callbacks.MallocCount == 2 && callbacks.FreeCount == 2, "miniaudio native FLAC callback report observes canary damage during close");
	}

	void TestMiniaudioFlacCallbackInitialSeekFailure (const std::vector<unsigned char> &seektableFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), false, false, 0, (std::size_t)-1, 0, 46, &callbacks) && callbacks.Opened && callbacks.SeekpointCount == 0 && callbacks.SeekCount == 2 && callbacks.SeekPositions[0] == 64 && callbacks.SeekPositions[1] == 46 && callbacks.FreeCount == 1, "miniaudio native FLAC first seektable seek failure disables table");
	}

	void TestMiniaudioFlacCallbackReturnSeekFailure (const std::vector<unsigned char> &seektableFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), false, false, 0, (std::size_t)-1, 0, 64, &callbacks) && !callbacks.Opened && callbacks.SeekCount == 3 && callbacks.SeekPositions[0] == 64 && callbacks.SeekPositions[1] == 46 && callbacks.SeekPositions[2] == 64 && callbacks.FreeCount == 1 && callbacks.CanaryIntact, "miniaudio native FLAC seektable recovery seek failure frees parent once");
	}

	void TestMiniaudioFlacCallbackShortRead (const std::vector<unsigned char> &seektableFlac)
	{
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		Check (AudioDecoderTestMiniaudioFlacCallbackOpen (&seektableFlac[0], seektableFlac.size (), false, false, 0, 46, 0, (std::size_t)-1, &callbacks) && callbacks.Opened && callbacks.SeekpointCount == 0 && callbacks.FreeCount == 1 && callbacks.ReadCount > 0 && callbacks.ReadPositions[callbacks.ReadCount - 1] == 46, "miniaudio native FLAC short seekpoint wire read disables table");
	}

	void TestMiniaudioFlacCallbackScenarios ()
	{
		const unsigned char seekpoint[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0x13, 0x57 };
		std::vector<unsigned char> flac;
		std::vector<unsigned char> seektableFlac;
		std::vector<unsigned char> emptySeektableFlac;
		std::vector<unsigned char> shortSeektableFlac;
		unsigned long long firstPCMFrame = 0;
		unsigned long long flacFrameOffset = 0;
		unsigned int pcmFrameCount = 0;
		unsigned int nativePCMFrameCount = 0;
		unsigned int metadataRawDataSize = 0;
		AudioDecoderTestMiniaudioFlacCallbackReport callbacks;
		if (ReadFileBytes (FixturePath ("flac_mono.flac").c_str (), &flac) && flac.size () > 42)
		{
			seektableFlac.assign (flac.begin (), flac.begin () + 42);
			seektableFlac[4] &= 0x7f;
			seektableFlac.push_back (0x83);
			seektableFlac.push_back (0);
			seektableFlac.push_back (0);
			seektableFlac.push_back (18);
			seektableFlac.insert (seektableFlac.end (), seekpoint, seekpoint + sizeof (seekpoint));
			seektableFlac.insert (seektableFlac.end (), flac.begin () + 42, flac.end ());
			TestMiniaudioFlacCallbackWireScenarios (seektableFlac, shortSeektableFlac, &firstPCMFrame, &flacFrameOffset, &pcmFrameCount, &nativePCMFrameCount, &metadataRawDataSize);
			emptySeektableFlac.assign (flac.begin (), flac.begin () + 42);
			emptySeektableFlac[4] &= 0x7f;
			emptySeektableFlac.push_back (0x83);
			emptySeektableFlac.push_back (0);
			emptySeektableFlac.push_back (0);
			emptySeektableFlac.push_back (0);
			emptySeektableFlac.insert (emptySeektableFlac.end (), flac.begin () + 42, flac.end ());
			TestMiniaudioFlacCallbackMetadataScenarios (seektableFlac, emptySeektableFlac, &callbacks);
			TestMiniaudioFlacCallbackAllocatorScenarios (seektableFlac, &callbacks);
			TestMiniaudioFlacCallbackFailureScenarios (seektableFlac, &callbacks);
			TestMiniaudioFlacCallbackMetadataParentAllocationOOM (seektableFlac);
			TestMiniaudioFlacCallbackMetadataShortRead (seektableFlac);
			TestMiniaudioFlacCallbackCanaryDamage (seektableFlac);
			TestMiniaudioFlacCallbackInitialSeekFailure (seektableFlac);
			TestMiniaudioFlacCallbackReturnSeekFailure (seektableFlac);
			TestMiniaudioFlacCallbackShortRead (seektableFlac);
			shortSeektableFlac = seektableFlac;
			shortSeektableFlac.resize (42 + 4 + 17);
			Check (!AudioDecoderTestMiniaudioFlacOpenSeektable (&shortSeektableFlac[0], shortSeektableFlac.size (), true, &pcmFrameCount, &firstPCMFrame, &flacFrameOffset, &nativePCMFrameCount, &metadataRawDataSize), "miniaudio native FLAC metadata seektable rejects short read");
			seektableFlac[45] = 17;
			Check (!AudioDecoderTestMiniaudioFlacOpenSeektable (&seektableFlac[0], seektableFlac.size (), false, &pcmFrameCount, &firstPCMFrame, &flacFrameOffset, &nativePCMFrameCount, &metadataRawDataSize), "miniaudio native FLAC rejects malformed seektable length");
		}
	}

	void TestMiniaudioFlacAllocationLayout ()
	{
		std::vector<unsigned char> flac;
		std::vector<unsigned char> seektableFlac;
		AudioDecodedPCM16 nativePCM;
		TestMiniaudioFlacAllocationBounds ();
		TestMiniaudioFlacAllocationOwnership (flac);
		TestMiniaudioFlacNativeFixtureLayout (flac);
		TestMiniaudioFlacValidSeektable (flac, seektableFlac, nativePCM);
		TestMiniaudioOggFlacSeektable (seektableFlac, nativePCM);
		TestMiniaudioFlacSeekpointWire ();
		TestMiniaudioFlacCallbackScenarios ();
	}

	void TestVorbisMemorySeekBoundaries ()
	{
		const unsigned char data[] = { 0, 1, 2, 3 };
		const unsigned int locations[] = { 0, 3, 4, 0, 5, UINT_MAX };
		int success[sizeof (locations) / sizeof (locations[0])] = { 0 };
		int eof[sizeof (locations) / sizeof (locations[0])] = { 0 };
		unsigned int offsets[sizeof (locations) / sizeof (locations[0])] = { 0 };
		Check (stb_vorbis_test_memory_seek_sequence (data, sizeof (data), locations, sizeof (locations) / sizeof (locations[0]), success, eof, offsets) != 0, "Vorbis memory seek test setup");
		Check (success[0] && !eof[0] && offsets[0] == 0, "Vorbis memory seek offset zero");
		Check (success[1] && !eof[1] && offsets[1] == 3, "Vorbis memory seek final byte");
		Check (!success[2] && eof[2] && offsets[2] == 4, "Vorbis memory seek length rejected");
		Check (success[3] && !eof[3] && offsets[3] == 0, "Vorbis memory seek recovers after rejection");
		Check (!success[4] && eof[4] && offsets[4] == 4, "Vorbis memory seek past length rejected");
		Check (!success[5] && eof[5] && offsets[5] == 4, "Vorbis memory seek unsigned maximum rejected");
	}

	void TestVorbisTemporaryMemoryRequirements ()
	{
		unsigned int required = 0;
		Check (stb_vorbis_test_temp_memory_required (16, 8192, 2, 0, 8192, 1, 1, &required) && required == 1048704, "Vorbis type-2 maximum temporary memory");
		Check (stb_vorbis_test_temp_memory_required (16, 8192, 0, 0, 8192, 1, 1, &required) && required == 524416, "Vorbis type-0 temporary memory");
		Check (stb_vorbis_test_temp_memory_required (16, 8192, 1, 0, 8192, 1, 1, &required) && required == 524416, "Vorbis type-1 temporary memory");
		Check (stb_vorbis_test_temp_memory_required (16, 8192, 2, 8192, UINT_MAX, 1, 1, &required) && required == 16384, "Vorbis temporary memory clamp");
		Check (!stb_vorbis_test_temp_memory_required (16, 8192, 2, 0, 8192, 0, 1, &required), "Vorbis zero partition size rejected");
		Check (!stb_vorbis_test_temp_memory_required (16, INT_MAX, 2, 0, UINT_MAX, 1, 1, &required), "Vorbis temporary memory overflow rejected");
	}

	void TestVorbisResidueShortBlockLayout (std::vector<char> *arena, int arenaLength, int *logicalPartitions, int *rowCapacity, int *channel0Samples, int *channel1Samples, int *bitsConsumed)
	{
		memset (&(*arena)[arenaLength], 0xa5, 8);
		Check (stb_vorbis_test_decode_residue_layout (1, 2, 8192, 128, 4096, &(*arena)[0], arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed) && *logicalPartitions == 128 && *rowCapacity == 128 && *channel0Samples == 128 && *channel1Samples == 128 && *bitsConsumed == 320, "Vorbis type-1 short-block residue decodes each channel within the current frame");
		Check (memcmp (&(*arena)[arenaLength], "\xa5\xa5\xa5\xa5\xa5\xa5\xa5\xa5", 8) == 0, "Vorbis type-1 short-block residue preserves bounded arena canary");
	}

	void TestVorbisResidueMaximumType1Layout (std::vector<char> *arena, int arenaLength, int *logicalPartitions, int *rowCapacity, int *channel0Samples, int *channel1Samples, int *bitsConsumed)
	{
		memset (&(*arena)[0], 0, arenaLength);
		memset (&(*arena)[arenaLength], 0xa5, 8);
		Check (stb_vorbis_test_decode_residue_layout (1, 1, 8192, 4096, 4095, &(*arena)[0], arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed) && *logicalPartitions == 4095 && *rowCapacity == 4095 && *channel0Samples == 4095 && *bitsConsumed == 5119, "Vorbis type-1 residue excludes rounded classification padding from decode");
	}

	void TestVorbisResidueType2Layout (std::vector<char> *arena, int arenaLength, int *logicalPartitions, int *rowCapacity, int *channel0Samples, int *channel1Samples, int *bitsConsumed)
	{
		Check (stb_vorbis_test_decode_residue_layout (2, 2, 8192, 128, 4096, &(*arena)[0], arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed) && *logicalPartitions == 256 && *rowCapacity == 256 && *channel0Samples == 128 && *channel1Samples == 128 && *bitsConsumed == 320, "Vorbis type-2 short-block residue interleaves both channels within the current frame");
		Check (stb_vorbis_test_decode_residue_layout (2, 2, 8192, 4096, 4095, &(*arena)[0], arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed) && *logicalPartitions == 4095 && *rowCapacity == 4095 && *channel0Samples == 2048 && *channel1Samples == 2047 && *bitsConsumed == 5119, "Vorbis type-2 residue excludes rounded classification padding from decode");
	}

	void TestVorbisResidueType0Layout (std::vector<char> *arena, int arenaLength, int *logicalPartitions, int *rowCapacity, int *channel0Samples, int *channel1Samples, int *bitsConsumed)
	{
		Check (stb_vorbis_test_decode_residue_layout (0, 2, 8192, 128, 4096, &(*arena)[0], arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed) && *logicalPartitions == 128 && *rowCapacity == 128 && *bitsConsumed == 320, "Vorbis type-0 residue preserves packet consumption and bounds coverage");
		Check (stb_vorbis_test_decode_residue_layout (0, 1, 8192, 4096, 4095, &(*arena)[0], arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed) && *logicalPartitions == 4095 && *rowCapacity == 4095 && *bitsConsumed == 5119, "Vorbis type-0 residue preserves logical packet consumption without a PCM oracle");
	}

	void TestVorbisResidueMaximumLayout (std::vector<char> *arena, int arenaLength, int *logicalPartitions, int *rowCapacity, int *channel0Samples, int *channel1Samples, int *bitsConsumed)
	{
		TestVorbisResidueMaximumType1Layout (arena, arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed);
		TestVorbisResidueType2Layout (arena, arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed);
		TestVorbisResidueType0Layout (arena, arenaLength, logicalPartitions, rowCapacity, channel0Samples, channel1Samples, bitsConsumed);
		Check (memcmp (&(*arena)[arenaLength], "\xa5\xa5\xa5\xa5\xa5\xa5\xa5\xa5", 8) == 0, "Vorbis residue runtime fixture preserves bounded arena canary");
	}

	void TestVorbisResidueRuntimeLayout ()
	{
		const int arenaLength = 65536;
		std::vector<char> arena (arenaLength + 8, 0);
		int logicalPartitions = 0;
		int rowCapacity = 0;
		int channel0Samples = 0;
		int channel1Samples = 0;
		int bitsConsumed = 0;
		TestVorbisResidueShortBlockLayout (&arena, arenaLength, &logicalPartitions, &rowCapacity, &channel0Samples, &channel1Samples, &bitsConsumed);
		TestVorbisResidueMaximumLayout (&arena, arenaLength, &logicalPartitions, &rowCapacity, &channel0Samples, &channel1Samples, &bitsConsumed);
	}

	void TestVorbisPersistentScratch ()
	{
		std::vector<unsigned char> bytes;
		std::vector<char> arena;
		unsigned int arenaRequired = 0;
		int error = 0;
		int scratchFreeCount = 0;
		int allocationsStable = 0;
		int arenaOffsetStable = 0;
		std::string path = FixturePath ("vorbis_mono.ogg");
		Check (ReadFileBytes (path.c_str (), &bytes) && !bytes.empty (), "Vorbis persistent scratch fixture loads");
		if (bytes.empty ())
		{
			return;
		}
		Check (stb_vorbis_test_open_memory_scratch (&bytes[0], (int)bytes.size (), NULL, 0, 0, &arenaRequired, &error, &scratchFreeCount, &allocationsStable, NULL) && error == 0 && scratchFreeCount == 1 && allocationsStable, "Vorbis persistent scratch normal allocation");
		arena.assign (arenaRequired, 0);
		Check (!arena.empty () && stb_vorbis_test_open_memory_scratch (&bytes[0], (int)bytes.size (), &arena[0], (int)arena.size (), 0, NULL, &error, &scratchFreeCount, &allocationsStable, &arenaOffsetStable) && error == 0 && scratchFreeCount == 0 && arenaOffsetStable, "Vorbis persistent scratch exact arena");
		Check (arena.size () > 8 && !stb_vorbis_test_open_memory_scratch (&bytes[0], (int)bytes.size (), &arena[0], (int)arena.size () - 8, 0, NULL, &error, &scratchFreeCount, &allocationsStable, &arenaOffsetStable) && error == stb_vorbis_test_outofmem_error () && scratchFreeCount == 0 && arenaOffsetStable, "Vorbis persistent scratch undersized arena");
		Check (!stb_vorbis_test_open_memory_scratch (&bytes[0], (int)bytes.size (), NULL, 0, 1, NULL, &error, &scratchFreeCount, &allocationsStable, NULL) && error == stb_vorbis_test_outofmem_error () && scratchFreeCount == 0, "Vorbis persistent scratch allocation failure");
		Check (!stb_vorbis_test_open_memory_scratch (&bytes[0], (int)bytes.size (), NULL, 0, 2, NULL, &error, &scratchFreeCount, &allocationsStable, NULL) && error == stb_vorbis_test_outofmem_error () && scratchFreeCount == 1, "Vorbis persistent scratch handle allocation failure");
		Check (stb_vorbis_test_arena_deinit_offset (INT_MAX - 7, &arenaOffsetStable) && arenaOffsetStable, "Vorbis persistent scratch arena close preserves near-limit offset");
	}

	void TestTimeTags ()
	{
		struct TimeTagCase
		{
			const char *Text;
			bool AsSamples;
			unsigned long long Value;
		};
		const TimeTagCase cases[] = {
			{ "", true, 0 }, { "123", true, 123 }, { ":", false, 0 }, { ":20", false, 20000 },
			{ "0:20", false, 20000 }, { "1:02", false, 62000 }, { "00:00:20", false, 20000 }, { ":20.1", false, 20100 },
			{ ":20.12", false, 20120 }, { ":20.123999", false, 20123 }, { "::", false, 0 }
		};
		for (std::size_t index = 0; index < sizeof (cases) / sizeof (cases[0]); ++index)
		{
			AudioTimeTag value;
			Check (ParseAudioTimeTag (cases[index].Text, &value) && value.AsSamples == cases[index].AsSamples && value.Value == cases[index].Value, "time tag compatible form");
		}
		const char *invalid[] = { "1.0", "1:::", ":1.2.3", " 1", "1:2x", "18446744073709551616", "999999999999999999:0" };
		for (std::size_t index = 0; index < sizeof (invalid) / sizeof (invalid[0]); ++index)
		{
			AudioTimeTag value;
			Check (!ParseAudioTimeTag (invalid[index], &value), "time tag invalid or overflow rejected");
		}
		{
			bool asSamples = false;
			unsigned int value = 0;
			Check (ParseAudioTimeTagToUInt ("4294967295", &asSamples, &value) && asSamples && value == UINT_MAX, "legacy time tag accepts UINT_MAX samples");
			Check (!ParseAudioTimeTagToUInt ("4294967296", &asSamples, &value), "legacy time tag rejects UINT_MAX plus one");
			Check (ParseAudioTimeTagToUInt (":20.123", &asSamples, &value) && !asSamples && value == 20123, "legacy time tag time-unit parity");
		}
	}

	void TestVorbisLoopComments ()
	{
		const char *both[] = { "LOOP_START=10", "LOOP_END=20" };
		const char *legacyAliases[] = { "LOOPSTART=10", "LOOPEND=20" };
		const char *bidiOnly[] = { "LOOP_BIDI=1" };
		const char *startOnly[] = { "LOOP_START=10" };
		const char *endOnly[] = { "LOOP_END=20" };
		const char *invalid[] = { "LOOP_START=1.0", "LOOP_END=20" };
		const char *overflow[] = { "LOOP_START=18446744073709551615:0", "LOOP_END=20" };
		const char *reversed[] = { "LOOP_START=20", "LOOP_END=10" };
		const char *outOfRange[] = { "LOOP_START=10", "LOOP_END=101" };
		AudioFrameRange range;
		int diagnostic = 0;
		Check (AudioDecoderTestParseVorbisLoopComments (both, 2, true, 100, 1000, &range, &diagnostic) && diagnostic == 0 && range.Start == 10 && range.End == 20, "Vorbis LOOP_START and LOOP_END comments");
		Check (AudioDecoderTestParseVorbisLoopComments (legacyAliases, 2, true, 100, 1000, &range, &diagnostic) && diagnostic == 0 && range.Start == 10 && range.End == 20, "Vorbis LOOPSTART and LOOPEND aliases");
		Check (!AudioDecoderTestParseVorbisLoopComments (bidiOnly, 1, true, 100, 1000, &range, &diagnostic) && diagnostic == 0, "Vorbis LOOP_BIDI remains deferred without diagnostic");
		Check (AudioDecoderTestParseVorbisLoopComments (startOnly, 1, true, 100, 1000, &range, &diagnostic) && diagnostic == 0 && range.Start == 10 && range.End == 100, "Vorbis loop start completes to length");
		Check (AudioDecoderTestParseVorbisLoopComments (endOnly, 1, true, 100, 1000, &range, &diagnostic) && diagnostic == 0 && range.Start == 0 && range.End == 20, "Vorbis loop end completes from zero");
		Check (!AudioDecoderTestParseVorbisLoopComments (invalid, 2, true, 100, 1000, &range, &diagnostic) && diagnostic == -1000, "Vorbis malformed loop tag diagnostic");
		Check (!AudioDecoderTestParseVorbisLoopComments (overflow, 2, true, 100, 1000, &range, &diagnostic) && diagnostic == -1000, "Vorbis loop tag overflow diagnostic");
		Check (!AudioDecoderTestParseVorbisLoopComments (reversed, 2, true, 100, 1000, &range, &diagnostic) && diagnostic == -1000, "Vorbis reversed loop diagnostic");
		Check (!AudioDecoderTestParseVorbisLoopComments (outOfRange, 2, true, 100, 1000, &range, &diagnostic) && diagnostic == -1000, "Vorbis out-of-range loop diagnostic");
		Check (!AudioDecoderTestParseVorbisLoopComments (startOnly, 1, false, 0, 1000, &range, &diagnostic) && diagnostic == -1000, "Vorbis unknown-length loop diagnostic");
		Check (strcmp (AudioDecoderTestVorbisLoopDiagnosticString (), "invalid Vorbis loop tags") == 0, "Vorbis loop diagnostic string");
	}
}

int main ()
{
	const char *path = "audio_decoder_tests_input.bin";
	FILE *file = fopen (path, "wb");
	if (file == NULL)
	{
		return 1;
	}
	fwrite ("0123456789", 1, 10, file);
	fclose (file);

	TestMemoryOwnership ();
	TestFileSourceBoundaries (path);
	TestFileFailures (path);
	TestProbeContracts ();
	TestId3ProbeContracts ();
	TestDecoderContract ();
	TestWavDecoder ();
	TestMiniaudioWavConversionHelpers ();
	TestMiniaudioWavReaderBoundaries ();
	TestTemporaryFixtureCreationFailures ();
	TestMiniaudioAutoDetectionFallback ();
	TestCodecFixtures ();
	TestMiniaudioFlacAllocationLayout ();
	TestVorbisMemorySeekBoundaries ();
	TestVorbisTemporaryMemoryRequirements ();
	TestVorbisResidueRuntimeLayout ();
	TestVorbisPersistentScratch ();
	TestTimeTags ();
	TestVorbisLoopComments ();
	remove (path);
	return Failures == 0 ? 0 : 1;
}