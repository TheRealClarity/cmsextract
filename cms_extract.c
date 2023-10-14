/*
 * build : gcc cms_extract.c -o cms_extract
 */

#include "cs_blobs.h"
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fileread(FILE *fp, uint32_t offset, size_t rdsize, void *buf) {
  if (!buf)
    return;
  memset(buf, 0, rdsize);
  fseek(fp, offset, SEEK_SET);
  fread(buf, rdsize, 1, fp);
}

static int filewrite(void *buf, size_t size, char *outfile) {
  FILE *out = fopen(outfile, "w");
  if (!out) {
    printf("error opening %s\n", outfile);
    return -1;
  }
  fwrite(buf, size, 1, out);
  printf("wrote file: %s\n", outfile);
  fflush(out);
  fclose(out);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    printf("%s <in> <out>\n", argv[0]);
    return 0;
  }

  char *infile = argv[1];
  char *outfile = argv[2];

  size_t header_size = sizeof(struct mach_header_64);
  void *header_buf = malloc(header_size);

  FILE *fp = fopen(infile, "r");
  if (!fp) {
    printf("opening %s", infile);
    return -1;
  }

  fileread(fp, 0, header_size, header_buf);
  if (!header_buf) {
    printf("error allocating file buffer\n");
    fclose(fp);
    return -1;
  }

  uint32_t header_offset = 0;
  uint32_t *magic = header_buf;

  if (*magic == FAT_CIGAM) {
    printf("FAT mach-o detected!\n");

    /* read fat header */
    size_t fh_size = sizeof(struct fat_header);
    void *fh_buf = malloc(fh_size);
    fileread(fp, 0, fh_size, fh_buf);

    struct fat_header *fh = fh_buf;
    uint32_t fh_arch_offset = fh_size;

    for (int i = 0; i < ntohl(fh->nfat_arch); i++) {
      size_t fh_arch_sz = sizeof(struct fat_arch);
      void *fh_arch_buf = malloc(fh_arch_sz);

      /* get first arch from fat mach-o */
      fileread(fp, fh_arch_offset, fh_arch_sz, fh_arch_buf);
      struct fat_arch *fa = fh_arch_buf;
      printf("cpu_type: %#x\n", fa->cputype);

      /* hardcoding arm64 */
      if (ntohl(fa->cputype) == CPU_TYPE_ARM64) {
        header_offset = ntohl(fa->offset);
        fileread(fp, header_offset, header_size, header_buf);
        printf("offset: %#x\n", header_offset);
        break;
      }
      fh_arch_offset += fh_arch_sz;
    }
  }

  struct mach_header_64 *machoheader = header_buf;

  printf("mach magic %#x\n", machoheader->magic);

  uint32_t code_signature_offset = 0;
  size_t code_signature_sz = 0;
  uint32_t lc_offset = 0;

  for (int i = 0; i < machoheader->ncmds; i++) {
    size_t lc_size = sizeof(struct load_command);
    void *lc_buf = malloc(lc_size);
    fileread(fp, header_offset + header_size + lc_offset, lc_size, lc_buf);
    struct load_command *lc = lc_buf;

    if (lc->cmd == LC_CODE_SIGNATURE) {
      /* get load command */
      size_t le_dc_size = sizeof(struct linkedit_data_command);
      void *lc_cs_buf = malloc(le_dc_size);
      fileread(fp, header_offset + header_size + lc_offset, le_dc_size, lc_cs_buf);

      /* parse load command */
      struct linkedit_data_command *le_dc = lc_cs_buf;
      code_signature_offset = le_dc->dataoff;
      code_signature_sz = le_dc->datasize;
      break;
    }
    lc_offset += lc->cmdsize;
  }

  void *code_signature = malloc(code_signature_sz);

  /* read code signature */
  fileread(fp, header_offset + code_signature_offset, code_signature_sz, code_signature);
  // filewrite(code_signature, code_signature_sz, outfile);

  /* parse super blob */
  CS_SuperBlob *cs_blob = code_signature;
  uint32_t blob_count = ntohl(cs_blob->count);
  printf("magic %#x\n", ntohl(cs_blob->magic));
  printf("blob count %u\n", blob_count);

  uint32_t cmsblob_offset = 0;
  uint32_t cmsblob_end = 0;

  for (int i = 0; i < blob_count; i++) {
    uint32_t type = ntohl(cs_blob->index[i].type);
    uint32_t offset = ntohl(cs_blob->index[i].offset);
    printf("type %#x\n", type);
    printf("offset %#x\n", offset);

    /* if we're the CMS blob */
    if (type == CSSLOT_SIGNATURESLOT) {
      cmsblob_offset = offset;
      /* if the CMS blob is the last blob... */
      if (i == (blob_count - 1)) {
        cmsblob_end = ntohl(cs_blob->length);
      } else {
        /* else the cms blob end is just the offset of the next blob... */
        cmsblob_end = ntohl(cs_blob->index[i + 1].offset);
      }
    }
  }

  if (cmsblob_offset && cmsblob_end) {
    size_t cmsblob_sz = cmsblob_end - cmsblob_offset;
    void *cms_gb = malloc(cmsblob_sz);
    fileread(fp, header_offset + code_signature_offset + cmsblob_offset, cmsblob_sz, cms_gb);
    CS_GenericBlob *cmsblob = cms_gb;
    filewrite(cmsblob->data, ntohl(cmsblob->length), outfile);
  }

  free(code_signature);
  free(header_buf);
  fclose(fp);
  return 0;
}