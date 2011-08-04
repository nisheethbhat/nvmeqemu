/*
 * INTEL CONFIDENTIAL
 * Copyright 2011 - 2011 Intel Corporation All Rights Reserved.
 * The source code contained or described herein and all documents related to
 * the source code ("Material") are owned by Intel Corporation or its
 * suppliers or licensors. Title to the Material remains with Intel Corporation
 * or its suppliers and licensors. The Material contains trade secrets and
 * proprietary and confidential information of Intel or its suppliers and
 * licensors. The Material is protected by worldwide copyright and trade secret
 * laws and treaty provisions. No part of the Material may be used, copied,
 * reproduced, modified, published, uploaded, posted, transmitted, distributed,
 * or disclosed in any way without Intel's prior express written permission.
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 *
 * This file deals with functions used for reading config files for both NVME
 * and PCI space
 */



#include "nvme.h"
#include "nvme_debug.h"


static void update_var(char* , FILERead *, int*, int *);
static void update_val(char* , char*);
static void config_space_write(NVMEState *,  FILERead*, uint8_t,
    uint16_t);
static void read_file_line(FILE *, char *);

/*********************************************************************
    Function     :    read_config_file
    Description  :    Function for reading config files
    Return Type  :    int (0 : Success , 1 : Error)
    Arguments    :    FILE *    : Config File Pointer
                      NVMEState * : Pointer to NVME device state
                      uint8_t : Flag for type of config space
                              : 0 = PCI Config Space
                              : 1 = NVME Config Space
*********************************************************************/
int read_config_file(FILE *config_file , NVMEState *n, uint8_t flag)
{
    /* Temp array used to store a line in the file */
    char var_line[MAX_CHAR_PER_LINE];
    /* Start of Reg and End of Reg indicator */
    int eor_flag = 0, sor_flag = 0;

    /* Offset used for reads and writes*/
    uint16_t offset = 0;
    /* Recent Capabilities pointer value*/
    uint16_t link = 0;
    /* Temporary variable for bit masking */
    uint32_t temp_mask = 0;
    /* New Size of BAR0 */
    uint32_t new_size_bar0 = 0 ;
    /* Structure to retain values for <REG to </REG> */
    FILERead data;




    /* Read the configuration file line by line */

    if (fgets(var_line, MAX_CHAR_PER_LINE, config_file) == NULL) {
        LOG_NORM("Config File cannot be read\n");
        return 1;
    }
    do {
        update_var((char *)var_line, &data, &eor_flag, &sor_flag);
        if (sor_flag == 1) {
            do {
                /* Reading line after line between <REG and </REG> */
                if (fgets(var_line, MAX_CHAR_PER_LINE, config_file) == NULL) {
                    LOG_NORM("Wrong Format between <REG> and </REG>\n");
                    return 1;
                }
                update_var((char *)var_line, &data, &eor_flag, &sor_flag);
            } while (eor_flag != 1);

            LOG_DBG("Length Read : %u\n", data.len);
            LOG_DBG("Offset Read : %u\n", data.offset);
            LOG_DBG("Val Read : %u\n", data.val);
            LOG_DBG("RO Mask Read : %u\n", data.ro_mask);
            LOG_DBG("RW Mask Read : %u\n", data.rw_mask);
            LOG_DBG("RWC Mask Read : %u\n", data.rwc_mask);
            LOG_DBG("RWS Mask Read : %u\n", data.rws_mask);
            LOG_DBG("CFG NAME Read : %s\n", data.cfg_name);

            /* Logic for extracting dependancy between different capabilites
             * from file
             */
            if (!strcmp(data.cfg_name, "PCIHEADER")) {
                offset = 0;
                if (data.offset == 0x34) {
                    link = data.val;
                }
            } else if (!strcmp(data.cfg_name, "PMCAP")) {

                if (data.offset == 0) {
                    /* One time per CFG_NAME */
                    offset = link;
                }
                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) (data.val >> 8);
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }

            } else if (!strcmp(data.cfg_name, "MSICAP")) {
                if (data.offset == 0) {
                    /* One time initialization per CFG_NAME */
                    offset = link;
                    n->dev.cap_present = n->dev.cap_present | QEMU_PCI_CAP_MSI;
                    n->dev.msi_cap = offset;
                }

                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) (data.val >> 8);
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }
            } else if (!strcmp(data.cfg_name, "MSIXCAP")) {
                if (data.offset == 0) {
                    /* One time initialization per CFG_NAME */
                    offset = link;
                    n->dev.cap_present = n->dev.cap_present | QEMU_PCI_CAP_MSIX;
                    n->dev.msix_cap = (uint8_t) offset;

                    /* Add space for MSI-X structures */
                    if (!n->bar0_size) {
                        new_size_bar0 = MSIX_PAGE_SIZE;
                    } else if (n->bar0_size < MSIX_PAGE_SIZE) {
                        new_size_bar0 = MSIX_PAGE_SIZE;
                        new_size_bar0 = MSIX_PAGE_SIZE * 2;
                    } else {
                        new_size_bar0 = n->bar0_size * 2;
                    }
                    n->dev.msix_bar_size = new_size_bar0;
                } else if (data.offset == 2) {
                    /* Initializing the MSI-X Cap */
                    temp_mask = data.val & (uint32_t) MASK(11, 0);
                    if (temp_mask == 0) {
                        LOG_ERR("Wrong entry for Table Size inside MXC in\
                            Config File");
                        LOG_ERR("Deafulting the value to 1");
                        n->nvectors = 0;
                    } else {
                        n->nvectors = (temp_mask - 1);
                    }
                } else if (data.offset == 4) {
                    /* TODO:
                     * Add support for linking BAR's to Table BIR
                     * depending upon MTAB
                     */
                } else if (data.offset == 8) {
                    /* TODO:
                     * Add support for linking BAR's to PBA BIR
                     * depending upon MPBA
                     */
                }

                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) (data.val >> 8);
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }
            } else if (!strcmp(data.cfg_name, "PXCAP")) {
                if (data.offset == 0) {
                    /* One time initialization per CFG_NAME */
                    offset = link;
                    n->dev.cap_present = n->dev.cap_present |
                        QEMU_PCI_CAP_EXPRESS;
                }
                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) (data.val >> 8);
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }
            } else if (!strcmp(data.cfg_name, "AERCAP")) {
                if (data.offset == 0) {
                    /* One time per initialization CFG_NAME */
                    offset = 0x100;
                    /* exp_cap is 8 bits only in Qemu PCI code.
                     * It should be 12 bits
                     */
                    n->dev.exp.exp_cap = offset;
                }
                if (data.offset == 0 && data.len > 3) {
                    link = (uint16_t) (data.val >> 20);
                } else if (data.offset == 2) {
                    link = (uint8_t) (data.val >> 4);
                }
            }


            if (((data.offset + offset + data.len) > 0x1000) && (flag ==
                PCI_SPACE)) {
                LOG_ERR("Invlaid Offsets present in the Config file for \
                    PCI address space \n");
            } else if (((data.offset + offset + data.len) > 0xFFF) && (flag ==
                NVME_SPACE)) {
                LOG_ERR("Invlaid Offsets present in the Config file for \
                    NVME address space \n");
            } else {
                /* PCI / NVME space writes */
                config_space_write(n, &data, flag, offset);
            }

        }

        /* Reseting the data strucutre */
        data.len = 0;
        data.offset = 0;
        data.val = 0;
        data.ro_mask = 0;
        data.rw_mask = 0;
        data.rwc_mask = 0;
        data.rws_mask = 0;
        bzero(data.cfg_name, sizeof(data.cfg_name));
        /* Resetting the flags */
        sor_flag = 0;
        eor_flag = 0;
    } while (fgets(var_line, MAX_CHAR_PER_LINE, config_file) != NULL);

    return 0;
}

/*********************************************************************
    Function     :    read_file_path
    Description  :    Finding the path to the config file in
                      the system
    Return Type  :    int (0 : Success , 1 : Error)
    Arguments    :    char * : Points the array through which file
                               path is returned
                      uint8_t : Flag for type of config space
                              : 0 = PCI Config Space
                              : 1 = NVME Config Space
*********************************************************************/
int read_file_path(char *arr, uint8_t flag)
{
    FILE *temp_file;
    int sys_ret;
    time_t tm; /* Time used as dynamic file name */
    char *rnd; /* Points to the static buffer returned by asctime() */
    char cmd_arr[100]; /* Temp array to build the command */

    /* Creating dynamic file name */
    tm = time(NULL);
    rnd = asctime(localtime(&tm));

    if (flag == PCI_SPACE) {
        strcpy((char *)cmd_arr, "find -name 'NVME_device_PCI_config' 2>/dev/"
            "null 1>'");
        strcat((char *)cmd_arr, rnd);
        strcat((char *)cmd_arr, "'");
        LOG_NORM("Find command : %s", cmd_arr);
    } else if (flag == NVME_SPACE) {
        strcpy((char *)cmd_arr, "find -name 'NVME_device_NVME_config' 2>/dev/"
            "null 1>'");
        strcat((char *)cmd_arr, rnd);
        strcat((char *)cmd_arr, "'");
    }

    /* Logic to make the Path to config files dynamic */
    temp_file = fopen(rnd, "w+");
    if (temp_file == NULL) {
            LOG_ERR("Could not open the temporary file");
            return 0;
    } else {
        sys_ret = system(cmd_arr);
        if (WIFSIGNALED(sys_ret) &&
            (WTERMSIG(sys_ret) == SIGINT || WTERMSIG(sys_ret) == SIGQUIT)) {
            LOG_ERR("Config File path not found on the system");
        } else {
            /* Custom function written to eliminate /n /r charachters in
            * standard fgets output
            */
            read_file_line(temp_file, arr);
        }
        fclose(temp_file);

        /* Deleting the temp file used for noting down the path */

        bzero(cmd_arr, 100 * sizeof(char));
        strcpy((char *)cmd_arr, "rm -rf '");
        strcat((char *)cmd_arr, rnd);
        strcat((char *)cmd_arr, "'");
        LOG_NORM("Remove command : %s", cmd_arr);
        sys_ret = system(cmd_arr);

        if (sys_ret != 0) {
            LOG_ERR("Could not delete the temp file");
        }
        return 1;
    }
}


/*********************************************************************
    Function     :    read_file_line
    Description  :    Reading a single line in a file
    Return Type  :    void
    Arguments    :    FILE * : FILE pointer
                      char * : Pointer to array
*********************************************************************/
static void read_file_line(FILE *flp, char *arr)
{
    uint32_t char_val; /* Used to read char from file */
    uint8_t temp_cnt = 0;

    do {
        char_val = fgetc(flp);
        if (char_val != '\n' && char_val != '\r') {
            arr[temp_cnt] = (char)char_val;
            temp_cnt++;
        }
    } while (char_val != '\n' && char_val != '\r' && char_val != EOF);

    arr[temp_cnt++] = '\0';
}


/*********************************************************************
    Function     :    update_var
    Description  :    Reads data variables (TAGS) from each line
    Return Type  :    void
    Arguments    :    char *: Single Line of config file
                      FILERead *: Pointing to store the values
                                  extracted
                      int * : Pointer to Start of Reg Indicator
                      int * : Pointer to End of Reg Indicator
*********************************************************************/

static void update_var(char *ptr , FILERead *data, int *eor, int *sor)
{
    /* Temp arrays for extracting TAGS and values respectivley */
    char tags[MAX_CHAR_PER_LINE], val[MAX_CHAR_PER_LINE];
    /* Offset used for parsing */
    int offset = 0;

    /* Reading of Data Variables from each line */
    do {
        if (*ptr != ' ' && *ptr != '\t' && *ptr != '\n' && *ptr != '\r') {
            tags[offset] = (char)*ptr;
            offset++;
        }
        ptr++;
    } while (*ptr != '=' && *ptr != '>' && (offset < MAX_CHAR_PER_LINE));

    tags[offset] = '\0';

    /* Compare the TAGS and extract the values */
    if (!strcmp(tags, "</REG")) {
        *eor = 1;

    } else if (!strcmp(tags, "<REG")) {
        *sor = 1;

    } else if (!strcmp(tags, "LENGTH")) {
        /* Pointer pre-incremented to get rid of "=" sign
         * while passing onto update_val function
         */
        update_val(++ptr, val);
        data->len = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "OFFSET")) {
        update_val(++ptr, val);
        data->offset = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "VALUE")) {
        update_val(++ptr, val);
        data->val = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RO_MASK")) {
        update_val(++ptr, val);
        data->ro_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RW_MASK")) {
        update_val(++ptr, val);
        data->rw_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RWC_MASK")) {
        update_val(++ptr, val);
        data->rwc_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RWS_MASK")) {
        update_val(++ptr, val);
        data->rws_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "CFG_NAME")) {
        update_val(++ptr, val);
        strcpy(data->cfg_name, val);
    }

    return;
}

/*********************************************************************
    Function     :    update_val
    Description  :    Reads data values from each line
    Return Type  :    void
    Arguments    :    char * : Offset inside a single line in File
                      char * : Pointer to store the values extracted
*********************************************************************/
static void update_val(char *ptr, char *val)
{
    int offset = 0;

    /* Reading of Data Values from each line */
    do {
        if (*ptr != ' ' && *ptr != '\t' && *ptr != '\n' && *ptr != '\r') {
            val[offset] = (char)*ptr;
            offset++;
        }
        ptr++;
    } while (*ptr != '\0' && (offset < MAX_CHAR_PER_LINE));
    val[offset] = '\0';
}

/*********************************************************************
    Function     :    config_space_write
    Description  :    Function for config space writes
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to NVME device state
                      FILERead *  : Pointer to the data extracted from
                                    <REG> </REG> from config file
                      uint8_t : Flag for type of config space
                              : 0 = PCI Config Space
                              : 1 = NVME Config Space
                      uint16_t : Dynamic offset ex:CAP pointer
*********************************************************************/
static void config_space_write(NVMEState *n, FILERead* data, uint8_t flag,
    uint16_t offset)
{
    /* Pointer to config  space */
    uint8_t *conf = NULL;
    /* Pointers for masks */
    uint8_t *wmask = NULL, *used = NULL, *cmask = NULL,
        *w1cmask = NULL, *w1smask = NULL;
    uint8_t index; /* Index for loop */

    if (PCI_SPACE == flag) {
        conf = n->dev.config;
        wmask = n->dev.wmask; /* R/W set if 1 else RO*/
        used = n->dev.used;
        cmask = n->dev.cmask;
        w1cmask = n->dev.w1cmask; /* W1C cleared if 1 */

        /* Write to config Space */
        for (index = 0; index < data->len; data->val >>= 8,
            data->rw_mask >>= 8, data->rwc_mask >>= 8, index++) {
            conf[data->offset + offset + index] = data->val;
            wmask[data->offset + offset + index] = data->rw_mask;
            used[data->offset + offset + index] = (uint8_t)MASK(8, 0);
            cmask[data->offset + offset + index] = (uint8_t)MASK(8, 0);
            w1cmask[data->offset + offset + index] = data->rwc_mask;
        }
    } else if (NVME_SPACE == flag) {
        conf = n->cntrl_reg;
        wmask = n->rw_mask; /* R/W set if 1 */
        w1cmask = n->rwc_mask; /* W1C cleared if 1 */
        w1smask = n->rws_mask; /* W1S set if 1 */
        used = n->used_mask; /* Used /Resv */
        /* Write to config Space */
        for (index = 0; index < data->len; data->val >>= 8,
            data->rws_mask >>= 8, data->rw_mask >>= 8, data->rwc_mask >>= 8,
                index++) {
            conf[data->offset + offset + index] = data->val;
            wmask[data->offset + offset + index] = data->rw_mask;
            w1smask[data->offset + offset + index] = data->rws_mask;
            w1cmask[data->offset + offset + index] = data->rwc_mask;
            used[data->offset + offset + index] = (uint8_t)MASK(8, 0);
        }
    }
}
