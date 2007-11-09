/*  libasf - An Advanced Systems Format media file parser
 *  Copyright (C) 2006-2007 Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asf.h"
#include "asfint.h"
#include "utf.h"
#include "header.h"
#include "guid.h"
#include "byteio.h"
#include "debug.h"

/**
 * Finds an object with the corresponding GUID type from header object. If
 * not found, just returns NULL.
 */
static asf_object_t *
asf_header_get_object(asf_object_header_t *header, const guid_type_t type)
{
	asf_object_t *current;

	current = header->first;
	while (current) {
		if (current->type == type) {
			return current;
		}
		current = current->next;
	}

	return NULL;
}

/**
 * Reads the stream properties object's data into the equivalent
 * data structure, and stores it in stream_properties_t structure
 * with the equivalent stream type. Needs the stream properties
 * object data as its input.
 */
static int
asf_parse_header_stream_properties(asf_stream_properties_t *sprop,
                                   uint8_t *objdata,
                                   uint32_t objsize)
{
	guid_t guid;
	guid_type_t type;
	uint32_t datalen;
	uint8_t *data;

	if (objsize < 78) {
		return ASF_ERROR_INVALID_LENGTH;
	}

	asf_byteio_getGUID(&guid, objdata);
	type = asf_guid_get_stream_type(&guid);

	datalen = asf_byteio_getDWLE(objdata + 40);
	if (datalen > objsize - 78) {
		return ASF_ERROR_INVALID_LENGTH;
	}
	data = objdata + 54;

	if (type == GUID_STREAM_TYPE_EXTENDED) {
		/* FIXME: Need to find out what actually is here...
		          but for now we can just skip the extended part */
		if (datalen < 64)
			return ASF_ERROR_INVALID_LENGTH;

		data += 64;
		datalen -= 64;

		/* update the stream type with correct one */
		asf_byteio_getGUID(&guid, objdata);
		type = asf_guid_get_stream_type(&guid);
	}

	switch (type) {
	case GUID_STREAM_TYPE_AUDIO:
	case GUID_STREAM_TYPE_EXTENDED_AUDIO:
	{
		asf_waveformatex_t *wfx;

		sprop->type = ASF_STREAM_TYPE_AUDIO;

		if (datalen < 18) {
			return ASF_ERROR_INVALID_LENGTH;
		}
		if (asf_byteio_getWLE(data + 16) > datalen - 16) {
			return ASF_ERROR_INVALID_LENGTH;
		}

		/* this should be freed in asf_close function */
		sprop->properties = malloc(sizeof(asf_waveformatex_t));
		if (!sprop->properties)
			return ASF_ERROR_OUTOFMEM;

		wfx = sprop->properties;
		wfx->codec_id = asf_byteio_getWLE(data);
		wfx->channels = asf_byteio_getWLE(data + 2);
		wfx->rate = asf_byteio_getDWLE(data + 4);
		wfx->bitrate = asf_byteio_getDWLE(data + 8);
		wfx->blockalign = asf_byteio_getWLE(data + 12);
		wfx->bitspersample = asf_byteio_getWLE(data + 14);
		wfx->datalen = asf_byteio_getWLE(data + 16);
		wfx->data = data + 18;

		if (wfx->datalen > datalen - 18) {
			debug_printf("Invalid waveformatex data length, truncating!");
			wfx->datalen = datalen - 18;
		}

		break;
	}
	case GUID_STREAM_TYPE_VIDEO:
	{
		asf_bitmapinfoheader_t *bmih;
		uint32_t width, height, flags, data_size;

		sprop->type = ASF_STREAM_TYPE_VIDEO;

		if (datalen < 51) {
			return ASF_ERROR_INVALID_LENGTH;
		}

		width = asf_byteio_getDWLE(data);
		height = asf_byteio_getDWLE(data + 4);
		flags = data[8];
		data_size = asf_byteio_getWLE(data + 9);

		data += 11;
		datalen -= 11;

		if (asf_byteio_getDWLE(data) != datalen) {
			return ASF_ERROR_INVALID_LENGTH;
		}
		if (width != asf_byteio_getDWLE(data + 4) ||
		    height != asf_byteio_getDWLE(data + 8) ||
		    flags != 2) {
			return ASF_ERROR_INVALID_VALUE;
		}

		/* this should be freed in asf_close function */
		sprop->properties = malloc(sizeof(asf_bitmapinfoheader_t));
		if (!sprop->properties)
			return ASF_ERROR_OUTOFMEM;

		bmih = sprop->properties;
		bmih->data_size = asf_byteio_getDWLE(data);
		bmih->width = asf_byteio_getDWLE(data + 4);
		bmih->height = asf_byteio_getDWLE(data + 8);
		bmih->reserved = asf_byteio_getDWLE(data + 12);
		bmih->bpp = asf_byteio_getDWLE(data + 14);
		bmih->codec = asf_byteio_getDWLE(data + 16);
		bmih->image_size = asf_byteio_getDWLE(data + 20);
		bmih->hppm = asf_byteio_getDWLE(data + 24);
		bmih->vppm = asf_byteio_getDWLE(data + 28);
		bmih->colors = asf_byteio_getDWLE(data + 32);
		bmih->important_colors = asf_byteio_getDWLE(data + 36);
		bmih->data = data + 40;

		if (bmih->data_size > datalen) {
			debug_printf("Invalid bitmapinfoheader data length, truncating!");
			bmih->data_size = datalen;
		}

		break;
	}
	case GUID_STREAM_TYPE_COMMAND:
		sprop->type = ASF_STREAM_TYPE_COMMAND;
		break;
	default:
		sprop->type = ASF_STREAM_TYPE_UNKNOWN;
		break;
	}

	return 0;
}

/**
 * Reads the file properties object contents to the asf_file_t structure,
 * parses the useful values from stream properties object to the equivalent
 * stream properties info structure and validates that all known header
 * subobjects have only legal values.
 */
int
asf_parse_header_validate(asf_file_t *file, asf_object_header_t *header)
{
	/* some flags for mandatory subobjects */
	int fileprop = 0, streamprop = 0;
	asf_object_t *current;

	if (header->first) {
		current = header->first;
		while (current) {
			uint64_t size = current->size;

			switch (current->type) {
			case GUID_FILE_PROPERTIES:
			{
				uint32_t max_packet_size;
				if (size < 104)
					return ASF_ERROR_OBJECT_SIZE;

				if (fileprop) {
					/* multiple file properties objects not allowed */
					return ASF_ERROR_INVALID_OBJECT;
				}

				fileprop = 1;
				asf_byteio_getGUID(&file->file_id, current->data);
				file->file_size = asf_byteio_getQWLE(current->data + 16);
				file->creation_date = asf_byteio_getQWLE(current->data + 24);
				file->data_packets_count = asf_byteio_getQWLE(current->data + 32);
				file->play_duration = asf_byteio_getQWLE(current->data + 40);
				file->send_duration = asf_byteio_getQWLE(current->data + 48);
				file->preroll = asf_byteio_getQWLE(current->data + 56);
				file->flags = asf_byteio_getDWLE(current->data + 64);
				file->packet_size = asf_byteio_getDWLE(current->data + 68);
				file->max_bitrate = asf_byteio_getQWLE(current->data + 76);

				max_packet_size = asf_byteio_getDWLE(current->data + 72);
				if (file->packet_size != max_packet_size) {
					/* in ASF file minimum packet size and maximum
					 * packet size have to be same apparently...
					 * stupid, eh? */
					return ASF_ERROR_INVALID_VALUE;
				}
				break;
			}
			case GUID_STREAM_PROPERTIES:
			{
				uint16_t flags;

				if (size < 78)
					return ASF_ERROR_OBJECT_SIZE;

				streamprop = 1;
				flags = asf_byteio_getWLE(current->data + 48);

				if (file->streams[flags & 0x7f].type) {
					/* only one stream object per stream allowed */
					return ASF_ERROR_INVALID_OBJECT;
				} else {
					asf_stream_properties_t *sprop;
					int ret;

					sprop = file->streams + (flags & 0x7f);

					ret = asf_parse_header_stream_properties(sprop,
					                                         current->data,
					                                         size);

					if (ret < 0) {
						return ret;
					}
				}
				break;
			}
			case GUID_CONTENT_DESCRIPTION:
			{
				uint32_t stringlen = 0;

				if (size < 34)
					return ASF_ERROR_OBJECT_SIZE;

				stringlen += asf_byteio_getWLE(current->data);
				stringlen += asf_byteio_getWLE(current->data + 2);
				stringlen += asf_byteio_getWLE(current->data + 4);
				stringlen += asf_byteio_getWLE(current->data + 6);
				stringlen += asf_byteio_getWLE(current->data + 8);

				if (size < stringlen + 34) {
					/* invalid string length values */
					return ASF_ERROR_INVALID_LENGTH;
				}
				break;
			}
			case GUID_MARKER:
				break;
			case GUID_CODEC_LIST:
				if (size < 44)
					return ASF_ERROR_OBJECT_SIZE;
				break;
			case GUID_STREAM_BITRATE_PROPERTIES:
				if (size < 26)
					return ASF_ERROR_OBJECT_SIZE;
				break;
			case GUID_PADDING:
				break;
			case GUID_EXTENDED_CONTENT_DESCRIPTION:
				if (size < 26)
					return ASF_ERROR_OBJECT_SIZE;
				break;
			case GUID_UNKNOWN:
				/* unknown guid type */
				break;
			default:
				/* identified type in wrong place */
				return ASF_ERROR_INVALID_OBJECT;
			}

			current = current->next;
		}
	}

	if (header->ext) {
		current = header->ext->first;
		while (current) {
			uint64_t size = current->size;

			switch (current->type) {
			case GUID_METADATA:
				if (size < 26)
					return ASF_ERROR_OBJECT_SIZE;
				break;
			case GUID_LANGUAGE_LIST:
				if (size < 26)
					return ASF_ERROR_OBJECT_SIZE;
				break;
			case GUID_EXTENDED_STREAM_PROPERTIES:
			{
				int stream, name_count, extsys_count;
				uint32_t datalen;
				uint8_t *data;
				uint16_t flags;
				int i;

				if (size < 88)
					return ASF_ERROR_OBJECT_SIZE;

				stream = asf_byteio_getWLE(current->data + 48);
				name_count = asf_byteio_getWLE(current->data + 60);
				extsys_count = asf_byteio_getWLE(current->data + 62);

				datalen = size - 88;
				data = current->data + 64;

				/* iterate through all name strings */
				for (i=0; i<name_count; i++) {
					uint16_t strlen;

					if (datalen < 4)
						return ASF_ERROR_INVALID_VALUE;

					strlen = asf_byteio_getWLE(data + 2);
					if (strlen > datalen) {
						return ASF_ERROR_INVALID_LENGTH;
					}

					/* skip the current name string */
					data += 4 + strlen;
					datalen -= 4 + strlen;
				}

				/* iterate through all extension systems */
				for (i=0; i<extsys_count; i++) {
					uint32_t extsyslen;

					if (datalen < 22)
						return ASF_ERROR_INVALID_VALUE;

					extsyslen = asf_byteio_getDWLE(data + 18);
					if (extsyslen > datalen) {
						return ASF_ERROR_INVALID_LENGTH;
					}

					/* skip the current extension system */
					data += 22 + extsyslen;
					datalen -= 22 + extsyslen;
				}

				if (datalen > 0) {
					guid_t guid;

					debug_printf("hidden stream properties object found!");

					/* this is almost same as in stream properties handler */
					if (datalen < 78)
						return ASF_ERROR_OBJECT_SIZE;

					/* check that we really have a stream properties object */
					asf_byteio_getGUID(&guid, data);
					if (asf_guid_get_type(&guid) != GUID_STREAM_PROPERTIES)
						return ASF_ERROR_INVALID_OBJECT;
					if (asf_byteio_getQWLE(data + 16) != datalen)
						return ASF_ERROR_OBJECT_SIZE;

					flags = asf_byteio_getWLE(data + 72);

					if ((flags & 0x7f) != stream ||
					    file->streams[stream].type) {
						/* only one stream object per stream allowed and
						 * stream ids have to match with both objects*/
						return ASF_ERROR_INVALID_OBJECT;
					} else {
						asf_stream_properties_t *sprop;
						int ret;

						sprop = file->streams + stream;

						ret = asf_parse_header_stream_properties(sprop,
											 data + 24,
											 datalen);

						if (ret < 0) {
							return ret;
						}
					}
				}
				break;
			}
			case GUID_ADVANCED_MUTUAL_EXCLUSION:
				if (size < 42)
					return ASF_ERROR_OBJECT_SIZE;
				break;
			case GUID_STREAM_PRIORITIZATION:
				if (size < 26)
					return ASF_ERROR_OBJECT_SIZE;
				break;
			case GUID_UNKNOWN:
				/* unknown guid type */
				break;
			default:
				/* identified type in wrong place */
				break;
			}

			current = current->next;
		}
	}

	if (!fileprop || !streamprop || !header->ext) {
		/* mandatory subobject missing */
		return ASF_ERROR_INVALID_OBJECT;
	}

	return 1;
}

/**
 * Destroy the header and all subobjects
 */
void
asf_header_destroy(asf_object_header_t *header)
{
	if (!header)
		return;

	if (header->first) {
		asf_object_t *current = header->first, *next;
		while (current) {
			next = current->next;
			free(current);
			current = next;
		}
	}

	if (header->ext) {
		asf_object_t *current = header->ext->first, *next;
		while (current) {
			next = current->next;
			free(current);
			current = next;
		}
	}
	free(header->data);
	free(header->ext);
	free(header);
}

/**
 * Allocates a metadata struct and parses the contents from
 * the header object raw data. All strings are in UTF-8 encoded
 * format. The returned struct needs to be freed using the
 * asf_header_metadata_destroy function. Returns NULL on failure.
 */
asf_metadata_t *
asf_header_get_metadata(asf_object_header_t *header)
{
	asf_object_t *current;
	asf_metadata_t *ret;

	/* allocate the metadata struct */
	ret = calloc(1, sizeof(asf_metadata_t));
	if (!ret) {
		return NULL;
	}

	current = asf_header_get_object(header, GUID_CONTENT_DESCRIPTION);
	if (current) {
		char *str;
		uint16_t strlen;
		int i, read = 0;

		/* The validity of the object is already checked so we can assume
		 * there's always enough data to read and there are no overflows */
		for (i=0; i<5; i++) {
			strlen = asf_byteio_getWLE(current->data + i*2);
			if (!strlen)
				continue;

			str = asf_utf8_from_utf16le(current->data + 10 + read, strlen);
			read += strlen;

			switch (i) {
			case 0:
				ret->title = str;
				break;
			case 1:
				ret->artist = str;
				break;
			case 2:
				ret->copyright = str;
				break;
			case 3:
				ret->description = str;
				break;
			case 4:
				ret->rating = str;
				break;
			default:
				free(str);
				break;
			}
		}
	}

	current = asf_header_get_object(header, GUID_EXTENDED_CONTENT_DESCRIPTION);
	if (current) {
		int i, j, position;

		ret->extended_count = asf_byteio_getWLE(current->data);
		ret->extended = calloc(ret->extended_count, sizeof(asf_metadata_entry_t));
		if (!ret->extended) {
			/* Clean up the already allocated parts and return */
			free(ret->title);
			free(ret->artist);
			free(ret->copyright);
			free(ret->description);
			free(ret->rating);
			free(ret);

			return NULL;
		}

		position = 2;
		for (i=0; i<ret->extended_count; i++) {
			uint16_t length, type;

			length = asf_byteio_getWLE(current->data + position);
			position += 2;

			ret->extended[i].key = asf_utf8_from_utf16le(current->data + position, length);
			position += length;

			type = asf_byteio_getWLE(current->data + position);
			position += 2;

			length = asf_byteio_getWLE(current->data + position);
			position += 2;

			switch (type) {
			case 0:
				/* type of the value is a string */
				ret->extended[i].value = asf_utf8_from_utf16le(current->data + position, length);
				break;
			case 1:
				/* type of the value is a data block */
				ret->extended[i].value = malloc((length*2 + 1) * sizeof(char));
				for (j=0; j<length; j++) {
					static const char hex[16] = "0123456789ABCDEF";
					ret->extended[i].value[j*2+0] = hex[current->data[position]>>4];
					ret->extended[i].value[j*2+1] = hex[current->data[position]&0x0f];
				}
				ret->extended[i].value[j*2] = '\0';
				break;
			case 2:
				/* type of the value is a boolean */
				ret->extended[i].value = malloc(6 * sizeof(char));
				sprintf(ret->extended[i].value, "%s",
				        *current->data ? "true" : "false");
				break;
			case 3:
				/* type of the value is a signed 32-bit integer */
				ret->extended[i].value = malloc(11 * sizeof(char));
				sprintf(ret->extended[i].value, "%u",
				        asf_byteio_getDWLE(current->data + position));
				break;
			case 4:
				/* FIXME: This doesn't print whole 64-bit integer */
				ret->extended[i].value = malloc(21 * sizeof(char));
				sprintf(ret->extended[i].value, "%u",
				        (uint32_t) asf_byteio_getQWLE(current->data + position));
				break;
			case 5:
				/* type of the value is a signed 16-bit integer */
				ret->extended[i].value = malloc(6 * sizeof(char));
				sprintf(ret->extended[i].value, "%u",
				        asf_byteio_getWLE(current->data + position));
				break;
			default:
				/* Unknown value type... */
				ret->extended[i].value = NULL;
				break;
			}
			position += length;
		}
	}

	return ret;
}

/**
 * Free the metadata struct and all fields it includes
 */
void
asf_header_metadata_destroy(asf_metadata_t *metadata)
{
	int i;

	free(metadata->title);
	free(metadata->artist);
	free(metadata->copyright);
	free(metadata->description);
	free(metadata->rating);
	for (i=0; i<metadata->extended_count; i++) {
		free(metadata->extended[i].key);
		free(metadata->extended[i].value);
	}
	free(metadata->extended);
	free(metadata);
}
