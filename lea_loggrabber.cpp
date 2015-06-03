/******************************************************************************/
/* fw1-loggrabber - (C)2005 Torsten Fellhauer, Xiaodong Lin                   */
/******************************************************************************/
/* Version: 1.11.1                                                            */
/******************************************************************************/
/*                                                                            */
/* Copyright (c) 2005 Torsten Fellhauer, Xiaodong Lin                         */
/* All rights reserved.                                                       */
/*                                                                            */
/* Redistribution and use in source and binary forms, with or without         */
/* modification, are permitted provided that the following conditions         */
/* are met:                                                                   */
/* 1. Redistributions of source code must retain the above copyright          */
/*    notice, this list of conditions and the following disclaimer.           */
/* 2. Redistributions in binary form must reproduce the above copyright       */
/*    notice, this list of conditions and the following disclaimer in the     */
/*    documentation and/or other materials provided with the distribution.    */
/*                                                                            */
/* THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND     */
/* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE      */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE    */
/* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL */
/* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS    */
/* OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)      */
/* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT */
/* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY  */
/* OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF     */
/* SUCH DAMAGE.                                                               */
/*                                                                            */
/******************************************************************************/
/* Description:                                                               */
/*                                                                            */
/* fw1-loggrabber is a simple LEA-Client which utilizes Checkpoints' OPSEC    */
/* SDK. It get any kind of Checkpoint FW-1 Log information from the Fire-     */
/* wall using the LEA-protocol.                                               */
/*                                                                            */
/* In order to use this program, you have to enable unauthorized connections  */
/* to your firewall within fwopsec.conf. Since Version 1.2 you can also use   */
/* authenticated and 3DES-encrypted connections.                              */
/* The current version also enables the usage of filter rule and an online    */
/* mode.                                                                      */
/*                                                                            */
/******************************************************************************/

#include "lea_loggrabber.h"
#include <errno.h>
#include <limits.h>
#include <sstream>
#include <exception>
#include <map>
#include <iostream>
#include <ctime>
#include <vector>

using namespace std;

const char* SPLUNKD_HTTP_STATUS_PREFIX = "HTTP Status: ";
const char* SPLUNKD_HTTP_PREFIX = "FAILED: 'HTTP/1.1 ";
const unsigned int HTTP_CODE_LEN = 3;
const unsigned int HTTP_NOT_FOUND = 404;

static void addFilter(configvalues& cfgvalues, const char* filter)
{
    if (cfgvalues.debug_mode)
    {
        fprintf(stderr, "addFilter: %s\n", filter);
    }
    
    cfgvalues.fw1_filter_count++;
    cfgvalues.fw1_filter_array =
        (char **) realloc (cfgvalues.fw1_filter_array,
                           cfgvalues.fw1_filter_count *
                           sizeof (char *));
    if (cfgvalues.fw1_filter_array == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }
    cfgvalues.fw1_filter_array[cfgvalues.fw1_filter_count - 1] = string_duplicate(filter);
}

static bool add_drop_vpn_rule(LeaFilterRulebase* rb)
{
    LeaFilterRule* dropVpnRule;
    if ((dropVpnRule = lea_filter_rule_create (LEA_FILTER_ACTION_DROP)) == NULL)
    {
        fprintf (stderr, "ERROR: failed to create drop vpn rule\n");
        lea_filter_rulebase_destroy (rb);
        return 0;
    }

    LeaFilterPredicate *dropVpnPred;
    int negation = 0;
    int argumentcount = 1;

    lea_value_ex_t* pVal[1];
    if ((pVal[0] = lea_value_ex_create())==NULL)
    {
        fprintf(stderr, "CreateRule2Pred: failed to create value\n");
        return 0;
    }

    int res = lea_value_ex_set(pVal[0], LEA_VT_STRING, "VPN-1");
    if (res!=OPSEC_SESSION_OK)
    {
        fprintf(stderr, "CreateRule2Pred: failed to set value\n");
        lea_value_ex_destroy(pVal[0]);
        return 0;
    }

    if ((dropVpnPred = lea_filter_predicate_create ("fw_subproduct", -1, negation,
            LEA_FILTER_PRED_BELONGS_TO, argumentcount, pVal)) == NULL)
    {
        fprintf (stderr, "ERROR: failed to create drop vpn predicate\n");
        lea_filter_rule_destroy (dropVpnRule);
        lea_filter_rulebase_destroy (rb);
        return 0;
    }

    if (lea_filter_rule_add_predicate(dropVpnRule, dropVpnPred)!=OPSEC_SESSION_OK)
    {
        fprintf(stderr, "Could not add dropVpnRule to dropVpnPred\n");
        lea_filter_rule_destroy(dropVpnRule);
        lea_filter_predicate_destroy(dropVpnPred);
        return 0;
    }

    if (lea_filter_rulebase_add_rule(rb, dropVpnRule)!=OPSEC_SESSION_OK)
    {
        fprintf(stderr, "Could not add dropVpnRule to rulebase rulebase\n");
        lea_filter_rulebase_destroy(rb);
        lea_filter_rule_destroy(dropVpnRule);
        return 0;
    } else {
        fprintf(stderr, "Added dropvpn rule successfully!\n");
    }

    lea_value_ex_destroy(pVal[0]);

    return 1;
}

bool launch_external_cmd(const string& command, string& result)
{
    const int BUF_SIZE = 1024;
    char buf[BUF_SIZE];
    bool ret = true;
    FILE* f = popen(command.c_str(), "r");
    if (!f)
    {
        return false;
    }
    while(!feof(f))
    {
        if(fgets(buf, BUF_SIZE, f))
        {
            try
            {
                result += buf;
            }
            catch (std::exception e)
            {
                ret = false;
                goto cleanup;
            }
        }
    }
cleanup:
    pclose(f);
    return ret;
}

bool IsHttpSuccess(unsigned int httpCode)
{
    if (httpCode < 300)
    {
        return true;
    }
    return false;
}

bool splunkd_internal_call(const string& command, string& result, unsigned int &httpCode)
{
    unsigned int match;
    httpCode = 0xffffffff;
    if (cfgvalues.debug_mode >= 1)
    {
        fprintf(stderr, "splunk internal call command: %s\n", command.c_str());
    }
    launch_external_cmd(command, result);
    if (cfgvalues.debug_mode >= 1)
    {
        fprintf(stderr, "splunk output: %s\n", result.c_str());
    }

    match = result.find(SPLUNKD_HTTP_STATUS_PREFIX);
    if (match != string::npos)
    {
        match += strlen(SPLUNKD_HTTP_STATUS_PREFIX);
        httpCode = atoi(result.substr(match, HTTP_CODE_LEN).c_str());
        if (IsHttpSuccess(httpCode))
        {
            return true;
        }
    }
    match = result.find(SPLUNKD_HTTP_PREFIX);
    if (match != string::npos)
    {
        match += strlen(SPLUNKD_HTTP_PREFIX);
        httpCode = atoi(result.substr(match, HTTP_CODE_LEN).c_str());
    }
    if (!IsHttpSuccess(httpCode))
    {
        if (cfgvalues.debug_mode >= 1)
        {
            fprintf(stderr, "splunkd request failed, %d: \n\t%s\n\t%s\n", httpCode, command.c_str(),
                    result.c_str());
        }
        return false;
    }
    return true;
}

string parseXml(const string& xml, const string& key)
{
    unsigned int startTag = xml.find("<s:key name=\""+key);
    if (startTag == string::npos)
    {
        return string("");
    }
    int startData = startTag + key.length() + strlen("<s:key name=\"") + strlen("\">");
    unsigned int endData = xml.find("</s:key>", startData);
    if (endData == string::npos)
    {
        return string("");
    }
    string data =  xml.substr(startData, endData-startData);
    string xmlDataStartTag = "<![CDATA[";
    string xmlDataEndTag = "]]>";
    unsigned int dataTagStartPos = data.find(xmlDataStartTag);
    if (dataTagStartPos == string::npos)
    {
        return data;
    }
    unsigned int dataTagEndPos = data.find(xmlDataEndTag);
    startData = dataTagStartPos + xmlDataStartTag.length();
    return data.substr(startData, dataTagEndPos-startData);
}

char* allocParam(stringstream& sstream)
{
    char* result = new char[sstream.str().length() + 1];
    strcpy(result, sstream.str().c_str());
    return result;
}

#define MAX_LEA_PARAM 256

bool getSplunkLeaConfigArgs(const string& entity, configvalues& cfgvalues,
                            int *argc, char***argv)
{
    cfgvalues.fw1_filter_count = 0;
    stringstream sstream;
    string response;
    unsigned int httpCode;
    int i = 0;

    string uri;
    if (cfgvalues.config_server != "")
    {
        uri.append(" -uri ");
        uri.append(cfgvalues.config_server);
    }
    sstream << "$SPLUNK_HOME/bin/splunk _internal call " << cfgvalues.config_endpoint.c_str()
            << entity.c_str() << uri;

    if (!splunkd_internal_call(sstream.str(), response, httpCode))
    {
        return false;
    }

    string mode = parseXml(response, "mode");
    fprintf(stderr, "mode: %s\n", mode.c_str());
    if(mode == "audit"){
        cfgvalues.audit_mode = 1;
        cfgvalues.fw_mode = 0;
    } else if (mode == "ips"){
        cfgvalues.ips_mode = 1;
        cfgvalues.fw_mode = 0;
    } else if (mode == "vpn"){
        cfgvalues.vpn_mode = 1;
        cfgvalues.fw_mode = 0;
    } else if (mode == "non_audit"){
        cfgvalues.non_audit_mode = 1;
        cfgvalues.fw_mode = 0;
    } else {
        cfgvalues.fw_mode = 1;
    }

    if(cfgvalues.ips_mode == 1)
    {
        addFilter(cfgvalues, "product=SmartDefense");
    } 
    else if(cfgvalues.vpn_mode == 1)
    {
        addFilter(cfgvalues, "fw_subproduct=VPN-1");
    }
    else if(cfgvalues.fw_mode == 1)
    {
        addFilter(cfgvalues, "product=VPN-1 & FireWall-1");
    }

    int no_resolve = atoi(parseXml(response, "no_resolve").c_str());
    if (no_resolve == 1)
    {
        cfgvalues.resolve_mode = FALSE;
    }
    else if (no_resolve == 0)
    {
        cfgvalues.resolve_mode = TRUE;
    }

    string online_mode = parseXml(response, "online_mode");
    if (atoi(online_mode.c_str()) == 1)
    {
        cfgvalues.online_mode = TRUE;
    }
    /*
     * if audit_mode set fw1_logfile to the correct setting
     */
    if (cfgvalues.audit_mode)
    {
        cfgvalues.fw1_logfile = string_duplicate ("fw.adtlog");
    }

    string no_nagle = parseXml(response, "no_nagle");
    if (no_nagle.size() > 0 && atoi(no_nagle.c_str()) == 1)
    {
        cfgvalues.no_nagle = TRUE;
    }

    string buf_size = parseXml(response, "conn_buf_size");
    if (buf_size.size() > 0)
    {
        int conn_buf_size = atoi(buf_size.c_str());
        if (conn_buf_size > 0)
        {
            cfgvalues.conn_buf_size = conn_buf_size;
        }
    }

    string clear_port = parseXml(response, "lea_server_port");
    int port = -1;
    if (clear_port.size() > 0)
    {
        port = atoi(clear_port.c_str());
    }

    *argv = new char*[MAX_LEA_PARAM];
    {
        stringstream sstream;
        sstream << "-v";
        (*argv)[i++] = allocParam(sstream);
    }
    {
        stringstream sstream;
        sstream << "opsec_sic_name" ;
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << parseXml(response, "opsec_sic_name");
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "-v";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "opsec_sslca_file" ;
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << parseXml(response, "opsec_sslca_file");
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "-v";
        (*argv)[i++] = allocParam(sstream);
    }
    {
        stringstream sstream;
        sstream << "lea_server";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "ip";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << parseXml(response, "lea_server_ip");
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "-v";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "lea_server";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "auth_port";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << parseXml(response, "lea_server_auth_port");
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "-v";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "lea_server";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "auth_type";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << parseXml(response, "lea_server_auth_type");
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "-v";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "lea_server";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << "opsec_entity_sic_name";
        (*argv)[i++] = allocParam(sstream);
    }

    {
        stringstream sstream;
        sstream << parseXml(response, "opsec_entity_sic_name");
        (*argv)[i++] = allocParam(sstream);
    }

    if (cfgvalues.conn_buf_size > 0)
    {
        {
            stringstream sstream;
            sstream << "-v";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << "lea_server";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << "conn_buf_size";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << cfgvalues.conn_buf_size;
            (*argv)[i++] = allocParam(sstream);
        }
    }

    if (cfgvalues.no_nagle)
    {
        {
            stringstream sstream;
            sstream << "-v";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << "lea_server";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << "no_nagle";
            (*argv)[i++] = allocParam(sstream);
        }
    }

    if (port >= 0)
    {
        {
            stringstream sstream;
            sstream << "-v";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << "lea_server";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << "port";
            (*argv)[i++] = allocParam(sstream);
        }

        {
            stringstream sstream;
            sstream << port;
            (*argv)[i++] = allocParam(sstream);
        }
    }

    *argc = i;

    if (cfgvalues.debug_mode)
    {
        for (int i = 0; i < *argc; i++)
        {
            fprintf(stderr, "%s ", (*argv)[i]);
        }
        fprintf(stderr, "\n");
    }

    return true;
}

bool deserializeUniqueStringList(string list, map<string, bool>& result)
{
    //TODO: properly tokenize
    istringstream iss(list);
    string element;
    while(getline(iss, element, ','))
    {
        result[element] = true;
    }
    return true;
}

bool serializeUniqueStringList(map<string, bool>& list, string& result)
{
    stringstream sstream;
    for (map<string, bool>::iterator it = list.begin(); it != list.end();)
    {
        sstream << it->first.c_str();
        it++;
        if (it != list.end())
        {
            sstream << ",";
        }
    }
    result = sstream.str();
    return true;
}

bool getStatus(const string& entity, const string& status_server,
               const string& log_status_endpoint,
               const string& status_server_auth_token,
               const int fileId, int& last_rec_pos)
{
    stringstream sstream;
    string response;
    unsigned int httpCode;
    char *endptr = NULL;
    last_rec_pos = -1;

    string uri;
    if (status_server != "")
    {
        uri.append(" -uri ");
        uri.append(status_server);
    }

    sstream << "$SPLUNK_HOME/bin/splunk _internal call " << log_status_endpoint.c_str() << fileId << "@"
            << entity.c_str()
            <<  uri ;
    if (!splunkd_internal_call(sstream.str(), response, httpCode))
    {
        if (httpCode == HTTP_NOT_FOUND)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    //get the last record position for that logfile
    string last_pos_str = parseXml(response, "last_rec_pos");
    if (last_pos_str.length() == 0)
    {
        fprintf(stderr, "getStatus: unable to retrieve last_rec_pos\n");
        return false;
    }

    errno = 0;
    last_rec_pos = strtol(last_pos_str.c_str(), &endptr, 0);

    if (last_rec_pos < INT_MIN || last_rec_pos > INT_MAX)
    {
        fprintf(stderr, "getStatus: unable to parse last_rec_pos %d %d\n", errno, last_rec_pos);
        return false;
    }
    if (errno == ERANGE && (last_rec_pos == LONG_MIN || last_rec_pos == LONG_MAX))
    {
        fprintf(stderr, "getStatus: unable to parse last_rec_pos %d %d\n", errno, last_rec_pos);
        return false;
    }
    if (endptr && *endptr != '\0')
    {
        fprintf(stderr, "getStatus: unable to parse last_rec_pos\n");
        return false;
    }
    return true;
}

bool postEntityLogStatus(const string& entity, const string& status_server,
                         const string& log_status_endpoint,
                         const string& status_server_auth_token,
                         lea_logdesc* logdesc, int last_rec_pos)
{
    stringstream sstream;
    string response;
    string logGuid;
    unsigned int httpCode;
    sstream << logdesc->fileid << "@" << entity.c_str();
    logGuid = sstream.str();

    //
    // update log file information
    //
    string uri;
    if (status_server != "")
    {
        uri.append(" -uri ");
        uri.append(status_server);
    }
    sstream.str( std::string() );
    sstream.clear();
    sstream << "$SPLUNK_HOME/bin/splunk _internal call " << log_status_endpoint.c_str()
            << uri << " -post:name " << logGuid.c_str() << " -post:fileid " << logdesc->fileid
            << " -post:filename " << logdesc->filename << " -post:last_rec_pos "  << last_rec_pos;

    if (!splunkd_internal_call(sstream.str(), response, httpCode))
    {
        return false;
    }

    if (cfgvalues.debug_mode >= 1)
    {
        fprintf(stderr,
                "Log position posted from REST: entity=%s status-server=%s log_status_endpoint=%s logFileName=%s logFileId=%d last_rec_pos=%d\n",
                entity.c_str(), status_server.c_str(), log_status_endpoint.c_str(),
                logdesc->filename, logdesc->fileid, last_rec_pos);
    }
    return true;
}

bool postEntityHealthStatus(const string& entity, const string& status_server,
                            const string& entity_health_endpoint,
                            const string& status_server_auth_token,
                            int is_connected)
{
    stringstream sstream;
    string response;
    unsigned int httpCode;

    string uri;
    if (status_server != "")
    {
        uri.append(" -uri ");
        uri.append(status_server);
    }

    //
    // update entity health
    //
    string lastConnectionTime;
    lastConnectionTime.append(" -post:last_connection_timestamp ");
    if (established)
    {
        time_t current_time;
        char buf[256];
        time(&current_time);
        strftime(buf, 256, "%FT%TZ", gmtime(&current_time));
        lastConnectionTime.append(buf);
    }
    else
    {
        sstream.str( std::string() );
        sstream.clear();

        sstream << "$SPLUNK_HOME/bin/splunk _internal call " << entity_health_endpoint.c_str() <<
                entity.c_str() <<  uri;
        splunkd_internal_call(sstream.str(), response, httpCode);

        //get the last timestamp and preserve it
        lastConnectionTime.append(parseXml(response, "last_connection_timestamp"));
    }
    sstream.str( std::string() );
    sstream.clear();
    sstream << "$SPLUNK_HOME/bin/splunk _internal call " << entity_health_endpoint.c_str()
            << uri << " -post:name " << entity.c_str() << " -post:is_connected " << is_connected
            << lastConnectionTime.c_str();
    return splunkd_internal_call(sstream.str(), response, httpCode);
}

/*
 * main function
 */
int
main (int argc, char *argv[])
{
    int i;
    int lfield_order_index;
    int afield_order_index;
    short amatch;
    short lmatch;
    int field_index;
    stringlist *lstptr;
    char *foundstring;
    char *field;
    char *fieldstring = NULL;
    string entity;

    /*
     * initialize field arrays
     */
    initialize_lfield_headers (lfield_headers);
    initialize_afield_headers (afield_headers);
    initialize_lfield_output (lfield_output);
    initialize_afield_output (afield_output);
    initialize_lfield_order (lfield_order);
    initialize_afield_order (afield_order);
    initialize_lfield_values (lfields);
    initialize_afield_values (afields);

    /* The default settings are shown as follows:
     * FW1_TYPE = NG release
     * Offline mode
     * Database is off
     * Print off log records
     */

    /*
     * process command line arguments
     */
    for (i = 1; i < argc; i++)
    {
        if (strcmp (argv[i], "--help") == 0)
        {
            usage (argv[0]);
            exit_loggrabber (1);
        }
        else if (strcmp (argv[i], "--help-fields") == 0)
        {
            show_supported_fields ();
            exit_loggrabber (1);
        }
        else if (strcmp (argv[i], "--resolve") == 0)
        {
            resolve_mode = 1;
        }
        else if ((strcmp (argv[i], "--noresolve") == 0)
                 || (strcmp (argv[i], "--no-resolve") == 0))
        {
            resolve_mode = 0;
        }
        else if (strcmp (argv[i], "--debug-level") == 0)
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (argv[i][0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            debug_mode = atoi (argv[i]);
        }
        else if (strcmp (argv[i], "--showfiles") == 0)
        {
            show_files = 1;
        }
        else if (strcmp (argv[i], "--showlogs") == 0)
        {
            show_files = 0;
        }
        else if (strcmp (argv[i], "--2000") == 0)
        {
            fw1_2000 = 1;
        }
        else if (strcmp (argv[i], "--ng") == 0)
        {
            fw1_2000 = 0;
        }
        else if (strcmp (argv[i], "--online") == 0)
        {
            online_mode = 1;
        }
        else if (strcmp (argv[i], "--no-online") == 0)
        {
            online_mode = 0;
        }
        else if (strcmp (argv[i], "--auditlog") == 0)
        {
            audit_log = 1;
        }
        else if (strcmp (argv[i], "--normallog") == 0)
        {
            audit_log = 0;
        }
        else if ((strcmp (argv[i], "-f") == 0)
                 || (strcmp (argv[i], "--logfile") == 0))
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (argv[i][0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            LogfileName = string_duplicate (argv[i]);
        }
        else if ((strcmp (argv[i], "-c") == 0)
                 || (strcmp (argv[i], "--configfile") == 0))
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (argv[i][0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            cfgvalues.config_filename = string_duplicate (argv[i]);
        }
        else if ((strcmp (argv[i], "-l") == 0)
                 || (strcmp (argv[i], "--leaconfigfile") == 0))
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (argv[i][0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            cfgvalues.leaconfig_filename = string_duplicate (argv[i]);
        }
        else if ((strcmp (argv[i], "--configserver") == 0))
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            cfgvalues.config_server = argv[i];
            if (cfgvalues.config_server.length() == 0 || cfgvalues.config_server[0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (cfgvalues.config_server[cfgvalues.config_server.length() - 1] != '/')
            {
                cfgvalues.config_server.append(1, '/');
            }
        }
        else if ((strcmp (argv[i], "--appname") == 0))
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            cfgvalues.app_name = argv[i];
            if (cfgvalues.app_name.length() == 0 || cfgvalues.app_name[0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
        }
        //TODO: support multiple entities
        else if ((strcmp (argv[i], "--configentity") == 0))
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            entity = argv[i];
            if (entity.length() == 0 || entity[0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
        }
        else if ((strcmp (argv[i], "--statusserver") == 0))
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            cfgvalues.status_server = argv[i];
            if (cfgvalues.status_server.length() == 0 || cfgvalues.status_server[0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (cfgvalues.status_server[cfgvalues.status_server.length() - 1] != '/')
            {
                cfgvalues.status_server.append(1, '/');
            }
        }
        else if (strcmp (argv[i], "--filter") == 0)
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (argv[i][0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            filtercount++;
            filterarray =
                (char **) realloc (filterarray, filtercount * sizeof (char *));
            if (filterarray == NULL)
            {
                fprintf (stderr, "ERROR: Out of memory\n");
                exit_loggrabber (1);
            }
            filterarray[filtercount - 1] = string_duplicate (argv[i]);
        }
        else if (strcmp (argv[i], "--fields") == 0)
        {
            i++;
            if (argv[i] == NULL)
            {
                fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            if (argv[i][0] == '-')
            {
                fprintf (stderr, "ERROR: Value expected for argument %s\n",
                         argv[i - 1]);
                usage (argv[0]);
                exit_loggrabber (1);
            }
            fieldstring = string_duplicate (argv[i]);
        }
        else
        {
            fprintf (stderr, "ERROR: Invalid argument: %s\n", argv[i]);
            usage (argv[0]);
            exit_loggrabber (1);
        }
    }


    /*
     * check configuration files
     */
    check_loggrabber_config (cfgvalues.config_filename);
    if (cfgvalues.app_name.length() == 0)
    {
        check_lea_config (cfgvalues.leaconfig_filename);
    }

    /*
     * load configuration file
     */
    read_config_file (cfgvalues.config_filename, &cfgvalues);

    // construct endpoints
    string endpoint_prefix = "/servicesNS/nobody/";
    string config_endpoint_postfix = "/opsec/opsec_conf/";
    string log_status_endpoint_postfix = "/opsec/log_status/";
    string entity_health_endpoint_postfix = "/opsec/entity_health/";
    //construct config endpoint
    cfgvalues.config_endpoint = endpoint_prefix;
    cfgvalues.config_endpoint += cfgvalues.app_name;
    cfgvalues.config_endpoint += config_endpoint_postfix;
    //construct log status endpoint
    cfgvalues.log_status_endpoint = endpoint_prefix;
    cfgvalues.log_status_endpoint += cfgvalues.app_name;
    cfgvalues.log_status_endpoint += log_status_endpoint_postfix;
    //construct entity health endpoint
    cfgvalues.entity_health_endpoint = endpoint_prefix;
    cfgvalues.entity_health_endpoint += cfgvalues.app_name;
    cfgvalues.entity_health_endpoint += entity_health_endpoint_postfix;

    /*
     * check whether command line options override configfile options
     */
    cfgvalues.debug_mode =
        (debug_mode != -1) ? debug_mode : cfgvalues.debug_mode;
    cfgvalues.online_mode =
        (online_mode != -1) ? online_mode : cfgvalues.online_mode;
    cfgvalues.resolve_mode =
        (resolve_mode != -1) ? resolve_mode : cfgvalues.resolve_mode;
    cfgvalues.fw1_2000 = (fw1_2000 != -1) ? fw1_2000 : cfgvalues.fw1_2000;
    cfgvalues.showfiles_mode =
        (show_files != -1) ? show_files : cfgvalues.showfiles_mode;
    cfgvalues.audit_mode = (audit_log != -1) ? audit_log : cfgvalues.audit_mode;
    cfgvalues.fieldnames_mode = TRUE;
    cfgvalues.fw1_logfile =
        (LogfileName !=
         NULL) ? string_duplicate (LogfileName) : cfgvalues.fw1_logfile;
    if (filtercount > 0)
    {
        cfgvalues.fw1_filter_count = filtercount;
        cfgvalues.fw1_filter_array = filterarray;
        cfgvalues.audit_filter_count = filtercount;
        cfgvalues.audit_filter_array = filterarray;
    }
    if ((!fieldstring) && (cfgvalues.fields))
    {
        fieldstring = string_duplicate (cfgvalues.fields);
    }

    // if fieldstring is NOT NULL, process this string
    if (fieldstring)
    {
        lfield_order_index = 0;
        afield_order_index = 0;

        while (fieldstring)
        {
            field = string_trim (string_get_token (&fieldstring, ';'), ' ');
            output_fields[field] = true;
        }
    }

    /*
     * free no more used char*
     */
    if (LogfileName != NULL)
    {
        free (LogfileName);
        LogfileName = NULL;
    }

    /*
     * if audit_mode set fw1_logfile to the correct setting
     */
    if (cfgvalues.audit_mode)
    {
        cfgvalues.fw1_logfile = string_duplicate ("fw.adtlog");
    }

    /*
     * perform validity check of given command line arguments
     */

    if (cfgvalues.fw1_2000)
    {
        if (cfgvalues.showfiles_mode)
        {
            fprintf (stderr,
                     "ERROR: --showfiles option is only available for connections\n"
                     "       to FW-1 NG. For connections to FW-1 4.1 (2000), please\n"
                     "       omit this parameter.\n");
            exit_loggrabber (1);
        }
        if ((cfgvalues.fw1_filter_count > 0)
                || (cfgvalues.audit_filter_count > 0))
        {
            fprintf (stderr,
                     "WARNING: --filter options are only available for connections\n"
                     "         to FW-1 NG. Available filterrules will be disabled!\n");
            cfgvalues.fw1_filter_count = 0;
            cfgvalues.audit_filter_count = 0;
        }
        if (cfgvalues.audit_mode)
        {
            fprintf (stderr,
                     "ERROR: --auditlog option is only available for connections\n"
                     "       to FW-1 NG. For connections to FW-1 4.1 (2000), please\n"
                     "       omit this parameter.\n");
            exit_loggrabber (1);
        }
    }

    if (cfgvalues.online_mode && (!(cfgvalues.audit_mode))
            && (strcmp (cfgvalues.fw1_logfile, "fw.log") != 0))
    {
        fprintf (stderr,
                 "ERROR: -f <FILENAME> option is not available in online mode. For use with Audit-Logfile, use --auditlog\n");
        exit_loggrabber (1);
    }

    if (cfgvalues.online_mode && cfgvalues.showfiles_mode)
    {
        fprintf (stderr,
                 "ERROR: --showfiles option is not available in online mode.\n");
        exit_loggrabber (1);
    }

    if (!(cfgvalues.audit_mode)
            && (strcmp (cfgvalues.fw1_logfile, "fw.adtlog") == 0))
    {
        fprintf (stderr,
                 "ERROR: use --auditlog option to get data of fw.adtlog\n");
        exit_loggrabber (1);
    }

    /*
     * set logging envionment
     */
    logging_init_env (cfgvalues.log_mode);

    open_log ();

    /*
     * set opsec debug level
     */
    opsec_set_debug_level (cfgvalues.debug_mode);

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Logfilename      : %s\n",
                 cfgvalues.fw1_logfile);
        fprintf (stderr, "DEBUG: Record Separator : %c\n",
                 cfgvalues.record_separator);
        fprintf (stderr, "DEBUG: Resolve Addresses: %s\n",
                 (cfgvalues.resolve_mode ? "Yes" : "No"));
        fprintf (stderr, "DEBUG: Show Filenames   : %s\n",
                 (cfgvalues.showfiles_mode ? "Yes" : "No"));
        fprintf (stderr, "DEBUG: FW1-2000         : %s\n",
                 (cfgvalues.fw1_2000 ? "Yes" : "No"));
        fprintf (stderr, "DEBUG: Online-Mode      : %s\n",
                 (cfgvalues.online_mode ? "Yes" : "No"));
        fprintf (stderr, "DEBUG: Audit-Log        : %s\n",
                 (cfgvalues.audit_mode ? "Yes" : "No"));
        fprintf (stderr, "DEBUG: Show Fieldnames  : %s\n",
                 (cfgvalues.fieldnames_mode ? "Yes" : "No"));
    }

    /*
     * function call to get available Logfile-Names (not available in FW1-4.1)
     */
    if (!(cfgvalues.fw1_2000) && !(cfgvalues.online_mode))
    {
        get_fw1_logfiles (entity);
    }

    if (cfgvalues.showfiles_mode)
    {
        if ((cfgvalues.fw1_2000) || (cfgvalues.online_mode))
        {
            fprintf (stderr,
                     "ERROR: Option --showfiles is not supported for Checkpoint FW-1 2000 or in online mode.\n");
        }
        close_log ();
        exit_loggrabber (0);
    }

    /*
     * process all logfiles if ALL specified
     */
    if (strcmp (cfgvalues.fw1_logfile, "ALL") == 0)
    {
        lstptr = sl;

        while (lstptr)
        {
            if (cfgvalues.debug_mode)
            {
                fprintf (stderr, "DEBUG: Processing Logfile: %s\n",
                         lstptr->data);
            }
            read_fw1_logfile (&(lstptr->data), entity, lstptr->normalFID);
            //TODO: Read the accounting id
            lstptr = lstptr->next;
        }
    }

    /*
     * else search for given string in available logfile-names
     */
    else
    {
        lstptr = stringlist_search (&sl, cfgvalues.fw1_logfile, &foundstring);

        /*
         * get the data from the matching logfiles
         */
        if (!lstptr)
        {
            if (cfgvalues.debug_mode)
            {
                fprintf (stderr, "DEBUG: Processing Logfile: %s\n",
                         cfgvalues.fw1_logfile);
            }
            read_fw1_logfile (&(cfgvalues.fw1_logfile), entity, LEA_NORMAL_FILEID);
        }
        while (lstptr)
        {
            if (cfgvalues.debug_mode)
            {
                fprintf (stderr, "DEBUG: Processing Logfile: %s\n",
                         foundstring);
            }
            read_fw1_logfile (&foundstring, entity, lstptr->normalFID);
            //TODO: read the account file id
            lstptr =
                stringlist_search (&(lstptr->next), cfgvalues.fw1_logfile,
                                   &foundstring);
        }
    }

    close_log ();

    exit_loggrabber (0);
    return (0);
}

/*
 * function read_fw1_logfile
 */
int
read_fw1_logfile (char **LogfileName, const string& entity, int fileid)
{
    OpsecEntity *pClient = NULL;
    OpsecEntity *pServer = NULL;
    OpsecSession *pSession = NULL;
    OpsecEnv *pEnv = NULL;
    LeaFilterRulebase *rb;
    int rbid = 1;
    int i, index;
    int opsecAlive;

    char *tmpstr1;
    char *message = NULL;
    unsigned int messagecap = 0;

    char *auth_type;
    char *fw1_server;
    char *fw1_port;
    char *opsec_certificate;
    char *opsec_client_dn;
    char *opsec_server_dn;
    int first = TRUE;

    char *(**headers);
    int *order;
    int number_fields;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile\n");
    }

    keepAlive = TRUE;

    while (keepAlive)
    {
        int last_rec_pos = -1;
        /* create opsec environment for the main loop */
        if (cfgvalues.app_name.length() > 0)
        {
            int argc = 0;
            char** argv = NULL;
            if (!getSplunkLeaConfigArgs(entity, cfgvalues,
                                        &argc, &argv))
            {
                fprintf (stderr, "ERROR: unable to get splunk lea config arguments (read_fw1_logfile)\n");
                exit_loggrabber (1, entity);
            }

            if (cfgvalues.audit_mode)
            {
                fileid = -1;
            }

            if ((pEnv =
                        opsec_init (OPSEC_CONF_ARGV, &argc, argv,
                                    OPSEC_EOL)) == NULL)
            {
                fprintf (stderr, "ERROR: unable to create environment (%s)\n",
                         opsec_errno_str (opsec_errno));
                exit_loggrabber (1, entity);
            }

        }
        else
        {
            if ((pEnv =
                        opsec_init (OPSEC_CONF_FILE, cfgvalues.leaconfig_filename,
                                    OPSEC_EOL)) == NULL)
            {
                fprintf (stderr, "ERROR: unable to create environment (%s)\n",
                         opsec_errno_str (opsec_errno));
                exit_loggrabber (1);
            }
        }

        if (cfgvalues.app_name.length() > 0)
        {
            if (!getStatus(entity, cfgvalues.status_server,
                           cfgvalues.log_status_endpoint,
                           cfgvalues.status_server_auth_token,
                           fileid, last_rec_pos))
            {
                fprintf (stderr, "ERROR: unable get progress (%s)\n",
                         opsec_errno_str (opsec_errno));
                exit_loggrabber (1, entity);
            }

            if (cfgvalues.debug_mode)
            {
                fprintf (stderr, "DEBUG: Starting %s %s %d at offset %d\n", entity.c_str(), *LogfileName, fileid,
                         last_rec_pos);
            }

        }

        if (cfgvalues.debug_mode)
        {

            fprintf (stderr, "DEBUG: OPSEC LEA conf file is %s\n",
                     cfgvalues.leaconfig_filename);

            fw1_server = opsec_get_conf (pEnv, "lea_server", "ip", NULL);
            if (fw1_server == NULL)
            {
                fprintf (stderr,
                         "ERROR: The fw1 server ip address has not been set.\n");
                exit_loggrabber (1, entity);
            }     //end of if
            auth_type = opsec_get_conf (pEnv, "lea_server", "auth_type", NULL);
            if (auth_type != NULL)
            {
                //Authentication mode
                if (cfgvalues.fw1_2000)
                {
                    //V4.1.2
                    fw1_port =
                        opsec_get_conf (pEnv, "lea_server", "auth_port", NULL);
                    if (fw1_port == NULL)
                    {
                        fprintf (stderr,
                                 "ERROR: The parameters about authentication mode have not been set.\n");
                        exit_loggrabber (1, entity);
                    }
                    else
                    {
                        fprintf (stderr,
                                 "DEBUG: Authentication mode has been used.\n");
                        fprintf (stderr, "DEBUG: Server-IP     : %s\n",
                                 fw1_server);
                        fprintf (stderr, "DEBUG: Server-Port     : %s\n",
                                 fw1_port);
                        fprintf (stderr, "DEBUG: Authentication type: %s\n",
                                 auth_type);
                    }   //end of inner if
                }
                else
                {
                    //NG
                    fw1_port =
                        opsec_get_conf (pEnv, "lea_server", "auth_port", NULL);
                    opsec_certificate =
                        opsec_get_conf (pEnv, "opsec_sslca_file", NULL);
                    opsec_client_dn =
                        opsec_get_conf (pEnv, "opsec_sic_name", NULL);
                    opsec_server_dn =
                        opsec_get_conf (pEnv, "lea_server",
                                        "opsec_entity_sic_name", NULL);
                    if ((fw1_port == NULL) || (opsec_certificate == NULL)
                            || (opsec_client_dn == NULL)
                            || (opsec_server_dn == NULL))
                    {
                        fprintf (stderr,
                                 "ERROR: The parameters about authentication mode have not been set.\n");
                        exit_loggrabber (1, entity);
                    }
                    else
                    {
                        fprintf (stderr,
                                 "DEBUG: Authentication mode has been used.\n");
                        fprintf (stderr, "DEBUG: Server-IP     : %s\n",
                                 fw1_server);
                        fprintf (stderr, "DEBUG: Server-Port     : %s\n",
                                 fw1_port);
                        fprintf (stderr, "DEBUG: Authentication type: %s\n",
                                 auth_type);
                        fprintf (stderr,
                                 "DEBUG: OPSEC sic certificate file name : %s\n",
                                 opsec_certificate);
                        fprintf (stderr, "DEBUG: Server DN (sic name) : %s\n",
                                 opsec_server_dn);
                        fprintf (stderr,
                                 "DEBUG: OPSEC LEA client DN (sic name) : %s\n",
                                 opsec_client_dn);
                    }   //end of inner if
                }
            }
            else
            {
                //Clear Text mode, i.e. non-auth mode
                fw1_port = opsec_get_conf (pEnv, "lea_server", "port", NULL);
                if (fw1_port != NULL)
                {
                    fprintf (stderr, "DEBUG: Clear text mode has been used.\n");
                    fprintf (stderr, "DEBUG: Server-IP        : %s\n",
                             fw1_server);
                    fprintf (stderr, "DEBUG: Server-Port      : %s\n",
                             fw1_port);
                }
                else
                {
                    fprintf (stderr,
                             "ERROR: The fw1 server lea service port has not been set.\n");
                    exit_loggrabber (1, entity);
                }   //end of inner if
            }     //end of middle if
        }     //end of if

        /*
         * initialize opsec-client
         */
        pClient = opsec_init_entity (pEnv, LEA_CLIENT,
                                     LEA_RECORD_HANDLER,
                                     read_fw1_logfile_record,
                                     LEA_DICT_HANDLER, read_fw1_logfile_dict,
                                     LEA_EOF_HANDLER, read_fw1_logfile_eof,
                                     LEA_SWITCH_HANDLER,
                                     read_fw1_logfile_switch,
                                     LEA_FILTER_QUERY_ACK,
                                     ((cfgvalues.
                                       audit_mode) ? ((cfgvalues.
                                               audit_filter_count >
                                               0) ?
                                               read_fw1_logfile_queryack
                                               : NULL) : ((cfgvalues.
                                                       fw1_filter_count
                                                       >
                                                       0) ?
                                                       read_fw1_logfile_queryack
                                                       : NULL)),
                                     LEA_COL_LOGS_HANDLER,
                                     read_fw1_logfile_collogs,
                                     LEA_SUSPEND_HANDLER,
                                     read_fw1_logfile_suspend,
                                     LEA_RESUME_HANDLER,
                                     read_fw1_logfile_resume,
                                     OPSEC_SESSION_START_HANDLER,
                                     read_fw1_logfile_start,
                                     OPSEC_SESSION_END_HANDLER,
                                     read_fw1_logfile_end,
                                     OPSEC_SESSION_ESTABLISHED_HANDLER,
                                     read_fw1_logfile_established, OPSEC_EOL);

        /*
         * initialize opsec-server for authenticated and unauthenticated connections
         */

        pServer =
            opsec_init_entity (pEnv, LEA_SERVER, OPSEC_ENTITY_NAME, "lea_server",
                               OPSEC_EOL);

        /*
         * continue only if opsec initializations were successful
         */
        if ((!pClient) || (!pServer))
        {
            fprintf (stderr,
                     "ERROR: failed to initialize client/server-pair (%s)\n",
                     opsec_errno_str (opsec_errno));
            cleanup_fw1_environment (pEnv, pClient, pServer);
            exit_loggrabber (1, entity);
        }

        /*
         * create LEA-session. differs for connections to FW-1 4.1 and FW-1 NG
         */
        if (cfgvalues.fw1_2000)
        {
            if (cfgvalues.online_mode)
            {
                pSession =
                    lea_new_session (pClient, pServer, LEA_ONLINE, LEA_FILENAME, *LogfileName, LEA_AT_END);
            }
            else
            {
                if (cfgvalues.debug_mode)
                {
                    fprintf (stderr, "DEBUG: Starting at position: %d\n", last_rec_pos);
                }
                if (last_rec_pos <= 0)
                {
                    pSession =
                        lea_new_session (pClient, pServer, LEA_OFFLINE, LEA_FILENAME,
                                         *LogfileName, LEA_AT_START);
                }
                else
                {
                    pSession = lea_new_session (pClient, pServer, LEA_OFFLINE,
                                                LEA_FILENAME, *LogfileName, LEA_AT_POS, last_rec_pos);

                    if (!pSession)
                    {
                        fprintf (stderr, "ERROR: could not establish connection starting at position (%d)\n",
                                 last_rec_pos);
                        cleanup_fw1_environment (pEnv, pClient, pServer);
                        exit_loggrabber (1, entity);
                    }

                }
            }
            if (!pSession)
            {
                fprintf (stderr, "ERROR: failed to create session (%s)\n",
                         opsec_errno_str (opsec_errno));
                cleanup_fw1_environment (pEnv, pClient, pServer);
                exit_loggrabber (1, entity);
            }
        }
        else
        {
            /*
             * create a suspended session, i.e. not log data will be sent to client
             */
            if (cfgvalues.online_mode)
            {
                pSession =
                    lea_new_suspended_session (pClient, pServer, LEA_ONLINE,
                                               LEA_FILENAME, *LogfileName,
                                               LEA_AT_END);
            }
            else
            {
                if (cfgvalues.debug_mode)
                {
                    fprintf (stderr, "DEBUG: Starting at position: %d\n", last_rec_pos);
                }

                if (last_rec_pos <= 0)
                {
                    if (cfgvalues.audit_mode)
                    {
                        pSession =
                            lea_new_suspended_session (pClient, pServer, LEA_OFFLINE,
                                                       LEA_FILENAME, *LogfileName,
                                                       LEA_AT_START);
                    }
                    else
                    {
                        pSession =
                            lea_new_suspended_session (pClient, pServer, LEA_OFFLINE,
                                                       LEA_UNIFIED_FILEID,
                                                       fileid,
                                                       LEA_AT_START);
                    }
                }
                else
                {

                    if (cfgvalues.audit_mode)
                    {
                        pSession =
                            lea_new_suspended_session (pClient, pServer, LEA_OFFLINE,
                                                       LEA_FILENAME, *LogfileName,
                                                       LEA_AT_POS, last_rec_pos);
                    }
                    else
                    {
                        pSession =
                            lea_new_suspended_session (pClient, pServer, LEA_OFFLINE,
                                                       LEA_UNIFIED_FILEID, fileid,
                                                       LEA_AT_POS, last_rec_pos);
                    }
                    if (!pSession)
                    {
                        fprintf (stderr, "ERROR: could not establish connection starting at position (%d)\n",
                                 last_rec_pos);
                        cleanup_fw1_environment (pEnv, pClient, pServer);
                        exit_loggrabber (1, entity);
                    }
                }
            }
            if (!pSession)
            {
                fprintf (stderr, "ERROR: failed to create session (%s)\n",
                         opsec_errno_str (opsec_errno));
                cleanup_fw1_environment (pEnv, pClient, pServer);
                exit_loggrabber (1, entity);
            }

            /*
             * If filters were defined, create the rulebase and register it.
             * the session will be resumed, as soon as the server sends the
             * filter_ack-event.
             * In the case when no filters are used, the suspended session
             * will be continued immediately.
             */
            if (cfgvalues.audit_mode)
            {
                if (cfgvalues.audit_filter_count > 0)
                {
                    if ((rb = lea_filter_rulebase_create ()) == NULL)
                    {
                        fprintf (stderr, "ERROR: failed to create rulebase\n");
                        exit_loggrabber (1, entity);
                    }

                    for (i = 0; i < cfgvalues.audit_filter_count; i++)
                    {
                        if ((rb =
                                    create_audit_filter_rule (rb,
                                                              cfgvalues.
                                                              audit_filter_array[i]))
                                == NULL)
                        {
                            fprintf (stderr, "ERROR: failed to create rule\n");
                            exit_loggrabber (1, entity);
                        }
                    }

                    if (lea_filter_rulebase_register (pSession, rb, &rbid) ==
                            LEA_FILTER_ERR)
                    {
                        fprintf (stderr, "ERROR: Cannot register rulebase\n");
                    }
                }
                else
                {
                    lea_session_resume (pSession);
                }
            }
            else
            {
                if (cfgvalues.fw1_filter_count > 0)
                {
                    if ((rb = lea_filter_rulebase_create ()) == NULL)
                    {
                        fprintf (stderr, "ERROR: failed to create rulebase\n");
                        exit_loggrabber (1, entity);
                    }

                    if(cfgvalues.fw_mode)
                    {
                        if(!add_drop_vpn_rule(rb))
                        {
                            fprintf(stderr, "ERROR: failed to create drop vpn rule\n");
                            exit_loggrabber(1,entity);
                        }
                    }

                    for (i = 0; i < cfgvalues.fw1_filter_count; i++)
                    {
                        if (cfgvalues.debug_mode)
                        {
                            fprintf(stderr, "filter %d: %s\n", 
                                i,
                                cfgvalues.fw1_filter_array[i]);
                        }

                        if ((rb =
                                    create_fw1_filter_rule (rb,
                                                            cfgvalues.
                                                            fw1_filter_array[i])) ==
                                NULL)
                        {
                            fprintf (stderr, "ERROR: failed to create rule\n");
                            exit_loggrabber (1,entity);
                        }
                    }

                    if (lea_filter_rulebase_register (pSession, rb, &rbid) ==
                            LEA_FILTER_ERR)
                    {
                        fprintf (stderr, "ERROR: Cannot register rulebase\n");
                    }
                }
                else
                {
                    lea_session_resume (pSession);
                }
            }
        }

        opsecAlive = opsec_start_keep_alive (pSession, 0);

        SESSION_CONTEXT sessionContext;
        sessionContext.config_server = cfgvalues.config_server;
        sessionContext.config_endpoint = cfgvalues.config_endpoint;
        sessionContext.config_server_auth_token = cfgvalues.status_server_auth_token;
        sessionContext.status_server = cfgvalues.status_server;
        sessionContext.log_status_endpoint = cfgvalues.log_status_endpoint;
        sessionContext.entity_health_endpoint = cfgvalues.entity_health_endpoint;
        sessionContext.status_server_auth_token = cfgvalues.status_server_auth_token;
        sessionContext.config_entity = entity;
        SESSION_OPAQUE(pSession) = &sessionContext;

        /*
         * start the opsec loop
         */
        opsec_mainloop (pEnv);

        /*
         * remove opsec stuff
         */
        cleanup_fw1_environment (pEnv, pClient, pServer);

        if (keepAlive)
        {
            SLEEP (recoverInterval);
        }
    }

    return 0;
}

/*
 * function read_fw1_logfile_queryack
 */
int
read_fw1_logfile_queryack (OpsecSession * psession, int filterID,
                           eLeaFilterAction filterAction, int filterResult)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_queryack\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: LEA logfile query ack handler was invoked\n");
    }
    lea_session_resume (psession);
    return OPSEC_SESSION_OK;
}

/*
 * function string_cat
 */
unsigned int
string_cat (char **dst, const char *src, unsigned int cap)
{
    char *buffer = NULL;
    unsigned int capacity = cap;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_cat\n");
    }

    // allocate memory for the string if NULL
    if (*dst == NULL)
    {
        *dst = (char *) malloc (INITIAL_CAPACITY + 1);
        if (*dst == NULL)
        {
            fprintf (stderr, "ERROR: Out of memory\n");
            exit_loggrabber (1);
        }
        **dst = '\0';
        capacity = INITIAL_CAPACITY;
    }

    // is src NULL, just return the capacity of dst
    if (!src)
    {
        return (capacity);
    }

    // is the capacity big enough for both src and dst?
    if (capacity >= strlen (*dst) + strlen (src))
    {
        strcat (*dst, src);
        return (capacity);
    }

    // otherwise define the new capacity
    capacity =
        ((capacity + CAPACITY_INCREMENT) >=
         (strlen (*dst) + strlen (src))) ? (capacity +
                                            CAPACITY_INCREMENT) : (strlen (*dst) +
                                                    strlen (src));

    // and allocate a temp buffer for dst
    buffer = (char *) malloc (strlen (*dst) + 1);
    if (!buffer)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }

    // copy dst to the tempbuffer and destroy dst
    strcpy (buffer, *dst);
    free (*dst);

    // recreate dst with the new capacity
    *dst = (char *) malloc (capacity + 1);
    if (*dst == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }

    // finally copy buffered dst and src back to dest and destroy temp buffer
    strcpy (*dst, buffer);
    free (buffer);
    strcat (*dst, src);

    // retrung new capacity
    return (capacity);
}

/*
 * function read_fw1_logfile_record
 */
int
read_fw1_logfile_record (OpsecSession * pSession, lea_record * pRec,
                         int pnAttribPerm[])
{
    char *szAttrib;
    char szNum[20];
    int i, j, match, index;
    unsigned long ul;
    unsigned short us;
    char tmpdata[16];
    time_t logtime;
    struct tm *datetime;
    char timestring[21];
    char *tmpstr1;
    char *tmpstr2;
    short first = TRUE;
    char *message = NULL;
    char *mymsg = NULL;
    char *(**headers);
    int *order;
    int num;
    int time;
    int number_fields;
    unsigned int messagecap = 0;
    int last_rec_pos = -1;
    vector<string> fields;
    vector<string> field_names;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_record\n");
    }

    if (cfgvalues.audit_mode)
    {
        num = AIDX_NUM;
        time = AIDX_TIME;
        number_fields = NUMBER_AIDX_FIELDS;
        headers = afield_headers;
        order = afield_order;
    }
    else
    {
        num = LIDX_NUM;
        time = LIDX_TIME;
        number_fields = NUMBER_LIDX_FIELDS;
        headers = lfield_headers;
        order = lfield_order;
    }

    /*
     * get record position
     */
    last_rec_pos = lea_get_record_pos (pSession) - 1;
    // preserve 1-based for output to splunk
    snprintf (szNum, sizeof(szNum), "%d", last_rec_pos+1);

    fields.push_back(szNum);
    field_names.push_back(*headers[num]);
    /*
     * process all fields of logentry
     */
    for (i = 0; i < pRec->n_fields; i++)
    {
        j = 0;
        match = FALSE;
        strcpy (tmpdata, "\0");
        szAttrib = lea_attr_name (pSession, pRec->fields[i].lea_attr_id);
        if (!szAttrib)
        {
            if (cfgvalues.debug_mode >= 2)
            {
                fprintf (stderr, "DEBUG: function read_fw1_logfile_record, lea_attr_name failed %d %d\n", i,
                         pRec->n_fields);
            }
            continue;
        }

        if (!(cfgvalues.resolve_mode))
        {
            switch (pRec->fields[i].lea_val_type)
            {
            /*
             * create dotted string of IP address. this differs between
             * Linux and Solaris.
             */
            case LEA_VT_IP_ADDR:
                ul = pRec->fields[i].lea_value.ul_value;
                if (BYTE_ORDER == LITTLE_ENDIAN)
                {
                    snprintf (tmpdata, sizeof(tmpdata), "%d.%d.%d.%d", (int) ((ul & 0xff) >> 0),
                              (int) ((ul & 0xff00) >> 8),
                              (int) ((ul & 0xff0000) >> 16),
                              (int) ((ul & 0xff000000) >> 24));
                }
                else
                {
                    snprintf (tmpdata, sizeof(tmpdata), "%d.%d.%d.%d",
                              (int) ((ul & 0xff000000) >> 24),
                              (int) ((ul & 0xff0000) >> 16),
                              (int) ((ul & 0xff00) >> 8),
                              (int) ((ul & 0xff) >> 0));
                }
                break;

            /*
             * print out the port number of the used service
             */
            case LEA_VT_TCP_PORT:
            case LEA_VT_UDP_PORT:
                us = pRec->fields[i].lea_value.ush_value;
                if (BYTE_ORDER == LITTLE_ENDIAN)
                {
                    us = (us >> 8) + ((us & 0xff) << 8);
                }
                snprintf (tmpdata, sizeof(tmpdata), "%d", us);
                break;
            }
        }

        if (strcmp (szAttrib, *headers[time]) == 0)
        {
            switch (cfgvalues.dateformat)
            {
            case DATETIME_CP:
                fields.push_back(lea_resolve_field(pSession, pRec->fields[i]));
                field_names.push_back(*headers[time]);
                break;
            case DATETIME_UNIX:
                snprintf (timestring, sizeof(timestring), "%lu",
                          pRec->fields[i].lea_value.ul_value);
                fields.push_back(timestring);
                field_names.push_back(*headers[time]);
                break;
            case DATETIME_STD:
                logtime = (time_t) pRec->fields[i].lea_value.ul_value;
                datetime = localtime (&logtime);
                strftime (timestring, 20, "%Y-%m-%d %H:%M:%S", datetime);
                fields.push_back(timestring);
                field_names.push_back(*headers[time]);
                break;
            default:
                fprintf (stderr, "ERROR: Unsupported dateformat chosen\n");
                exit_loggrabber (1);
            }
        }
        else
        {
            if (tmpdata[0])
            {
                fields.push_back(tmpdata);
                field_names.push_back(szAttrib);
            }
            else
            {
                fields.push_back(lea_resolve_field
                                 (pSession, pRec->fields[i]));
                field_names.push_back(szAttrib);
            }
        }
    }

    /*
     * print logentry to stdout
     */
    for (i = 0; i < fields.size(); i++)
    {
        // fieldname mode -> process only existing fields
        if (output_fields.size() == 0 || output_fields.find(field_names[i]) != output_fields.end())
        {
            tmpstr1 =
                string_escape (field_names[i].c_str(),
                               cfgvalues.record_separator);
            tmpstr2 =
                string_escape (fields[i].c_str(),
                               cfgvalues.record_separator);

            char* stringnumber;

            if (first)
            {
                // snprintf reserves 1 byte for the null terminator
                // Therefore another byte is added to guarantee there is enough room
                int size = 1 + snprintf(NULL, 0, "%s=%s", tmpstr1, tmpstr2);
                stringnumber = (char*) malloc(size);
                snprintf(stringnumber, size, "%s=%s", tmpstr1, tmpstr2);

                first = FALSE;
            }
            else
            {
                int size = 1 + snprintf(NULL, 0, "%c%s=%s",
                                        cfgvalues.record_separator, tmpstr1, tmpstr2);

                stringnumber = (char*) malloc(size + 1);
                snprintf(stringnumber, size, "%c%s=%s",
                         cfgvalues.record_separator, tmpstr1, tmpstr2);
            }

            messagecap = string_cat (&message, stringnumber, messagecap);

            free (tmpstr1);
            free (tmpstr2);
            free(stringnumber);
        }
    }

    if (cfgvalues.log_mode != ODBC)
    {
        if ((message != NULL) && (strlen (message) > 0))
        {
            mymsg = string_mask_newlines (message);
            submit_log (mymsg);
            free (message);
            free (mymsg);
        }
    }

    PSESSION_CONTEXT pContext = (PSESSION_CONTEXT) SESSION_OPAQUE(pSession);
    lea_logdesc *logdesc = NULL;
    if (pContext->config_entity.length() > 0 &&
            (last_rec_pos > 0 && (last_rec_pos%cfgvalues.splunkRestStatusCommit == 0)))
    {
        logdesc = lea_get_logfile_desc(pSession);
        if (logdesc)
        {
            postEntityLogStatus(pContext->config_entity, pContext->status_server, pContext->log_status_endpoint,
                                pContext->status_server_auth_token,
                                logdesc,  last_rec_pos);
            postEntityHealthStatus(pContext->config_entity, pContext->status_server,
                                   pContext->entity_health_endpoint,
                                   pContext->status_server_auth_token, established);
        }
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_dict
 */
int
read_fw1_logfile_dict (OpsecSession * psession, int dict_id, LEA_VT val_type,
                       int n_d_entries)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_dict\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: LEA logfile dict handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_eof
 */
int
read_fw1_logfile_eof (OpsecSession * psession)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_eof\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: LEA end of logfile handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_switch
 */
int
read_fw1_logfile_switch (OpsecSession * psession)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_switch\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: LEA logfile switch handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_collogs
 */
int
read_fw1_logfile_collogs (OpsecSession * psession)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_collogs\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: LEA collected logfile handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_suspend
 */
int
read_fw1_logfile_suspend (OpsecSession * psession)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_suspend\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: LEA suspended session handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_resume
 */
int
read_fw1_logfile_resume (OpsecSession * psession)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_resume\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: LEA resumed session handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function get_fw1_logfiles_end
 */
int
get_fw1_logfiles_end (OpsecSession * psession)
{
    int end_reason = 0;
    int sic_errno = 0;
    char *sic_errmsg = NULL;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function get_fw1_logfiles_end\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: OPSEC_SESSION_END_HANDLER called\n");
    }

    // Check what is the reason of opsec session end
    end_reason = opsec_session_end_reason (psession);
    switch (end_reason)
    {
    case END_BY_APPLICATION:
    case SESSION_TIMEOUT:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "DEBUG: The session has been ended.\n");
        }
        break;
    case SESSION_NOT_ENDED:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "WARNING: The session has not been terminated.\n");
        }
        break;
    case UNABLE_TO_ATTACH_COMM:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: Failed to establish connection.\n");
        }
        break;
    case ENTITY_TYPE_SESSION_INIT_FAIL:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr,
                     "ERROR: OPSEC API-specific library code on other side of connection failed when attempting to initialize the session.\n");
        }
        break;
    case ENTITY_SESSION_INIT_FAIL:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr,
                     "ERROR: Third-party start handler on other side of connection failed.\n");
        }
        break;
    case COMM_FAILURE:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: Communication failure.\n");
        }
        break;
    case BAD_VERSION:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: Incorrect version at other side.\n");
        }
        break;
    case PEER_SEND_DROP:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: The peer dropped the connection.\n");
        }
        break;
    case PEER_ENDED:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: The peer ends.\n");
        }
        break;
    case PEER_SEND_RESET:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: The peer reset the connection.\n");
        }
        break;
    case COMM_IS_DEAD:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: No communication.\n");
        }
        break;
    case SIC_FAILURE:
        if (!opsec_get_sic_error (psession, &sic_errno, &sic_errmsg))
        {
            if (cfgvalues.debug_mode)
            {
                fprintf (stderr, "ERROR: SIC ERROR %d - %s\n", sic_errno,
                         sic_errmsg);
            }
        }
        break;
    default:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "Warning: Unknown reason of session end.\n");
        }
        break;
    }       //end of switch

    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_end
 */
int
read_fw1_logfile_end (OpsecSession * psession)
{
    int end_reason = 0;
    int sic_errno = 0;
    char *sic_errmsg = NULL;

    int last_rec_pos = -1;
    PSESSION_CONTEXT pContext = (PSESSION_CONTEXT) SESSION_OPAQUE(psession);
    lea_logdesc *logdesc = NULL;

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: OPSEC_SESSION_END_HANDLER called\n");
    }

    // Check what is the reason of opsec session end
    end_reason = opsec_session_end_reason (psession);
    switch (end_reason)
    {
    case END_BY_APPLICATION:
    case SESSION_TIMEOUT:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "DEBUG: The session has been ended.\n");
        }
        keepAlive = FALSE;
        break;
    case SESSION_NOT_ENDED:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "WARNING: The session has not been terminated.\n");
        }
        keepAlive = FALSE;
        break;
    case UNABLE_TO_ATTACH_COMM:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: Failed to establish connection.\n");
        }
        if (established)
        {
            keepAlive = TRUE;
        }
        else
        {
            keepAlive = FALSE;
        };
        break;
    case ENTITY_TYPE_SESSION_INIT_FAIL:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr,
                     "ERROR: OPSEC API-specific library code on other side of connection failed when attempting to initialize the session.\n");
        }
        keepAlive = FALSE;
        break;
    case ENTITY_SESSION_INIT_FAIL:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr,
                     "ERROR: Third-party start handler on other side of connection failed.\n");
        }
        keepAlive = FALSE;
        break;
    case COMM_FAILURE:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: Communication failure.\n");
        }
        if (established)
        {
            keepAlive = TRUE;
        }
        else
        {
            keepAlive = FALSE;
        };
        break;
    case BAD_VERSION:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: Incorrect version at other side.\n");
        }
        keepAlive = FALSE;
        break;
    case PEER_SEND_DROP:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: The peer dropped the connection.\n");
        }
        if (established)
        {
            keepAlive = TRUE;
        }
        else
        {
            keepAlive = FALSE;
        };
        break;
    case PEER_ENDED:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: The peer ends.\n");
        }
        keepAlive = FALSE;
        break;
    case PEER_SEND_RESET:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: The peer reset the connection.\n");
        }
        if (established)
        {
            keepAlive = TRUE;
        }
        else
        {
            keepAlive = FALSE;
        };
        break;
    case COMM_IS_DEAD:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "ERROR: No communication.\n");
        }
        if (established)
        {
            keepAlive = TRUE;
        }
        else
        {
            keepAlive = FALSE;
        };
        break;
    case SIC_FAILURE:
        if (!opsec_get_sic_error (psession, &sic_errno, &sic_errmsg))
        {
            if (cfgvalues.debug_mode)
            {
                fprintf (stderr, "ERROR: SIC ERROR %d - %s\n", sic_errno,
                         sic_errmsg);
            }
        }
        keepAlive = FALSE;
        break;
    default:
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "Warning: Unknown reason of session end.\n");
        }
        keepAlive = FALSE;
        break;
    }   //end of switch

    if (pContext->config_entity.length() > 0)
    {
        last_rec_pos = lea_get_record_pos(psession);
        if (last_rec_pos <= 0)
        {
            if (cfgvalues.debug_mode)
            {
                fprintf (stderr,
                         "ERROR: Received error when trying to obtain last record number: function call lea_get_record_pos");
            }
        }
        else
        {
            logdesc = lea_get_logfile_desc(psession);
            if (cfgvalues.debug_mode && !logdesc)
            {
                fprintf (stderr,
                         "ERROR: Received error when trying to obtain log file descriptor: function call lea_get_logfile_desc");
            }
            else
            {
                int retryWait = 1;
                for (int i = 0; i < cfgvalues.splunkRestMaxRetries; i++)
                {
                    if (postEntityLogStatus(pContext->config_entity, pContext->status_server,
                                            pContext->log_status_endpoint,
                                            pContext->status_server_auth_token,
                                            logdesc,  last_rec_pos))
                    {
                        break;
                    }
                    sleep(retryWait);
                    retryWait = retryWait*cfgvalues.splunkRestRetryFactor;
                }
            }
        }
        postEntityHealthStatus(pContext->config_entity, pContext->status_server,
                               pContext->entity_health_endpoint,
                               pContext->status_server_auth_token, established);
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_start
 */
int
read_fw1_logfile_start (OpsecSession * psession)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_start\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: OPSEC session start handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_established
 */
int
read_fw1_logfile_established (OpsecSession * psession)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_established\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr,
                 "DEBUG: OPSEC session established handler was invoked\n");
    }
    established = TRUE;
    return OPSEC_SESSION_OK;
}

/*
 * function read_fw1_logfile_failedconn
 */
int
read_fw1_logfile_failedconn (OpsecEntity * entity, long peer_ip,
                             int sic_errno, char *sic_errmsg)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function read_fw1_logfile_failedconn\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr,
                 "DEBUG: OPSEC failed connection handler was invoked\n");
    }
    return OPSEC_SESSION_OK;
}

/*
 * function get_fw1_logfiles
 */
int
get_fw1_logfiles (const string& entity)
{
    OpsecEntity *pClient = NULL;
    OpsecEntity *pServer = NULL;
    OpsecSession *pSession = NULL;
    OpsecEnv *pEnv = NULL;
    int opsecAlive;

    char *auth_type;
    char *fw1_server;
    char *fw1_port;
    char *opsec_certificate;
    char *opsec_client_dn;
    char *opsec_server_dn;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function get_fw1_logfiles\n");
    }

    /* create opsec environment for the main loop */
    if (cfgvalues.app_name.length() > 0)
    {
        int argc = 0;
        char** argv = NULL;
        if (!getSplunkLeaConfigArgs(entity,
                                    cfgvalues,
                                    &argc, &argv))
        {
            fprintf (stderr, "ERROR: unable to get splunk lea config arguments(get_fw1_logfiles)\n");
            exit_loggrabber (1);

        }

        if ((pEnv =
                    opsec_init (OPSEC_CONF_ARGV, &argc, argv,
                                OPSEC_EOL)) == NULL)
        {
            fprintf (stderr, "ERROR: unable to create environment (%s)\n",
                     opsec_errno_str (opsec_errno));
            exit_loggrabber (1, entity);
        }

    }
    else
    {
        if ((pEnv =
                    opsec_init (OPSEC_CONF_FILE, cfgvalues.leaconfig_filename,
                                OPSEC_EOL)) == NULL)
        {
            fprintf (stderr, "ERROR: unable to create environment (%s)\n",
                     opsec_errno_str (opsec_errno));
            exit_loggrabber (1);
        }

    }

    if (cfgvalues.debug_mode)
    {

        fprintf (stderr, "DEBUG: OPSEC LEA conf file is %s\n",
                 cfgvalues.leaconfig_filename);

        fw1_server = opsec_get_conf (pEnv, "lea_server", "ip", NULL);
        if (fw1_server == NULL)
        {
            fprintf (stderr,
                     "ERROR: The fw1 server ip address has not been set.\n");
            exit_loggrabber (1, entity);
        }     //end of if
        auth_type = opsec_get_conf (pEnv, "lea_server", "auth_type", NULL);
        if (auth_type != NULL)
        {
            //Authentication mode
            fw1_port = opsec_get_conf (pEnv, "lea_server", "auth_port", NULL);
            opsec_certificate = opsec_get_conf (pEnv, "opsec_sslca_file", NULL);
            opsec_client_dn = opsec_get_conf (pEnv, "opsec_sic_name", NULL);
            opsec_server_dn =
                opsec_get_conf (pEnv, "lea_server", "opsec_entity_sic_name",
                                NULL);
            if ((fw1_port == NULL) || (opsec_certificate == NULL)
                    || (opsec_client_dn == NULL) || (opsec_server_dn == NULL))
            {
                fprintf (stderr,
                         "ERROR: The parameters about authentication mode have not been set.\n");
                exit_loggrabber (1, entity);
            }
            else
            {
                fprintf (stderr, "DEBUG: Authentication mode has been used.\n");
                fprintf (stderr, "DEBUG: Server-IP     : %s\n", fw1_server);
                fprintf (stderr, "DEBUG: Server-Port     : %s\n", fw1_port);
                fprintf (stderr, "DEBUG: Authentication type: %s\n", auth_type);
                fprintf (stderr,
                         "DEBUG: OPSEC sic certificate file name : %s\n",
                         opsec_certificate);
                fprintf (stderr, "DEBUG: Server DN (sic name) : %s\n",
                         opsec_server_dn);
                fprintf (stderr, "DEBUG: OPSEC LEA client DN (sic name) : %s\n",
                         opsec_client_dn);
            }     //end of inner if
        }
        else
        {
            //Clear Text mode, i.e. non-auth mode
            fw1_port = opsec_get_conf (pEnv, "lea_server", "port", NULL);
            if (fw1_port != NULL)
            {
                fprintf (stderr, "DEBUG: Clear text mode has been used.\n");
                fprintf (stderr, "DEBUG: Server-IP        : %s\n", fw1_server);
                fprintf (stderr, "DEBUG: Server-Port      : %s\n", fw1_port);
            }
            else
            {
                fprintf (stderr,
                         "ERROR: The fw1 server lea service port has not been set.\n");
                exit_loggrabber (1, entity);
            }     //end of inner if
        }     //end of middle if
    }       //end of if


    /*
     * initialize opsec-client
     */
    pClient = opsec_init_entity (pEnv, LEA_CLIENT,
                                 LEA_DICT_HANDLER, get_fw1_logfiles_dict,
                                 OPSEC_SESSION_END_HANDLER,
                                 get_fw1_logfiles_end, OPSEC_EOL);

    /*
     * initialize opsec-server for authenticated and unauthenticated connections
     */
    pServer =
        opsec_init_entity (pEnv, LEA_SERVER, OPSEC_ENTITY_NAME, "lea_server",
                           OPSEC_EOL);

    /*
     * continue only if opsec initializations were successful
     */
    if ((!pClient) || (!pServer))
    {
        fprintf (stderr,
                 "ERROR: failed to initialize client/server-pair (%s)\n",
                 opsec_errno_str (opsec_errno));
        cleanup_fw1_environment (pEnv, pClient, pServer);
        exit_loggrabber (1, entity);
    }

    /*
     * create LEA-session
     */
    if (!
            (pSession =
                 lea_new_session (pClient, pServer, LEA_OFFLINE, LEA_FILENAME,
                                  LEA_NORMAL, LEA_AT_START)))
    {
        fprintf (stderr, "ERROR: failed to create session (%s)\n",
                 opsec_errno_str (opsec_errno));
        cleanup_fw1_environment (pEnv, pClient, pServer);
        exit_loggrabber (1, entity);
    }

    opsecAlive = opsec_start_keep_alive (pSession, 0);

    /*
     * start the opsec loop
     */
    opsec_mainloop (pEnv);

    /*
     * remove opsec stuff
     */
    cleanup_fw1_environment (pEnv, pClient, pServer);

    return 0;
}

/*
 * function get_fw1_logfiles_dict
 */
int
get_fw1_logfiles_dict (OpsecSession * pSession, int nDictId, LEA_VT nValType,
                       int nEntries)
{
    int learesult = 0;
    int nID = 0;
    int aID = 0;
    char *logfile = NULL;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function get_fw1_logfiles_dict\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Available FW-1 Logfiles\n");
    }

    if (cfgvalues.showfiles_mode)
    {
        fprintf (stderr, "Available FW-1 Logfiles\n");
    }

    /*
     * get names of available logfiles and create list of these names
     */
    learesult = lea_get_first_file_info (pSession, &logfile, &nID, &aID);

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function get_fw1_logfiles_dict, lea_get_first_file_info returned %d\n",
                 learesult);
    }
    while (LEA_SESSION_FILE_PURGED == learesult || LEA_SESSION_OK == learesult)
    {
        if (cfgvalues.debug_mode)
        {
            fprintf (stderr, "DEBUG: - %s\n", logfile);
        }
        if (cfgvalues.showfiles_mode)
        {
            fprintf (stderr, "- %s\n", logfile);
        }
        stringlist_append (&sl, logfile, nID, aID);
        learesult = lea_get_next_file_info (pSession, &logfile, &nID, &aID);
        if (cfgvalues.debug_mode >= 2)
        {
            fprintf (stderr, "DEBUG: function get_fw1_logfiles_dict, lea_get_next_file_info returned %d\n",
                     learesult);
        }
    }

    /*
     * end opsec-session
     */
    opsec_end_session (pSession);

    return OPSEC_SESSION_OK;
}

/*
 * function cleanup_fw1_environment
 */
void
cleanup_fw1_environment (OpsecEnv * env, OpsecEntity * client,
                         OpsecEntity * server)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function cleanup_fw1_environment\n");
    }

    if (client)
        opsec_destroy_entity (client);
    if (server)
        opsec_destroy_entity (server);
    if (env)
        opsec_env_destroy (env);
}

/*
 * function show_supported_fields
 */
void
show_supported_fields ()
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function show_supported_fields\n");
    }

    fprintf (stderr,
             "\nFW1-Loggrabber v%s, (C)2004, Torsten Fellhauer, Xiaodong Lin\n",
             VERSION);
    fprintf (stderr, "Supported Fields for normal logs:\n");
    for (i = 0; i < NUMBER_LIDX_FIELDS; i++)
    {
        fprintf (stderr, "  - %s\n", *lfield_headers[i]);
    }
    fprintf (stderr, "Supported Fields for audit logs:\n");
    for (i = 0; i < NUMBER_AIDX_FIELDS; i++)
    {
        fprintf (stderr, "  - %s\n", *afield_headers[i]);
    }
}

/*
 * function usage
 */
void
usage (char *szProgName)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function usage\n");
    }

    fprintf (stderr, "\nFW1-Loggrabber v%s (%s)\n", VERSION, ODBCVERSION);
    fprintf (stderr, "    (C)2005, Torsten Fellhauer, Xiaodong Lin\n\n");
    fprintf (stderr, "Usage:\n");
    fprintf (stderr, " %s [ options ]\n", szProgName);
    fprintf (stderr,
             "  -c|--configfile <file>     : Name of Configfile (default: fw1-loggrabber.conf)\n");
    fprintf (stderr,
             "  -l|--leaconfigfile <file>  : Name of Leaconfigfile (default: lea.conf)\n");
    fprintf (stderr,
             "  -f|--logfile Logfile|ALL   : Name of Logfile (default: fw.log)\n");
    fprintf (stderr,
             "  --resolve|--no-resolve     : Resolve Port Numbers and IP-Addresses (Default: Resolve)\n");
    fprintf (stderr,
             "  --showfiles|--showlogs     : Show only Filenames of all available FW-1 Logfiles (default: showlogs)\n");
    fprintf (stderr,
             "  --2000|--ng                : Connect to a CP FW-1 4.1 (2000) (default is ng)\n");
    fprintf (stderr,
             "  --filter \"...\"             : Specify filters to be applied\n");
    fprintf (stderr,
             "  --fields \"...\"             : Specify fields to be printed\n");
    fprintf (stderr,
             "  --online|--no-online       : Enable Online mode (default: no-online)\n");
    fprintf (stderr,
             "  --auditlog|--normallog     : Get data of audit-logfile (fw.adtlog)(default: normallog)\n");
    fprintf (stderr,
             "  --debug-level <level>      : Specify Debuglevel (default: 0 - no debugging)\n");
    fprintf (stderr,
             "  --configentity <entity>    : Specifies the entity name in the Splunk opsec app endpoint to collect logs from\n");
    fprintf (stderr,
             "  --configserver <splunkd>   : optional, specifies the Splunk instance to get lea configuration from, e.g. https://127.0.0.1:8089/. defaults to instance in $SPLUNK_HOME\n");
    fprintf (stderr,
             "  --statusserver <splunkd>   : optional, specifies the Splunk instance to post status information to, e.g. https://127.0.0.1:8089/. defaults to instance in $SPLUNK_HOME\n");
    fprintf (stderr,
             "  --appname <app>            : Specifies the name of the splunk app on the Splunk server \n");
    fprintf (stderr,
             "  --help                     : Show usage informations\n");
    fprintf (stderr,
             "  --help-fields              : Show supported log fields\n");
}

/*
 * function stringlist_append
 */
int
stringlist_append (stringlist ** lst, char *data, int normalFID, int accountFID)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function stringlist_append\n");
    }

    /*
     * return if data is NULL
     */
    if (data == NULL)
    {
        return 0;
    }

    /*
     * move to last element of list
     */
    while (*lst)
        lst = &((*lst)->next);
    /*
     * allocate memory for new element
     */
    *lst = (stringlist *) malloc (sizeof (stringlist));
    if (*lst == NULL)
        return 0;
    /*
     * append new element
     */
    (*lst)->data = string_duplicate (data);
    (*lst)->normalFID = normalFID;
    (*lst)->accountFID = accountFID;
    (*lst)->next = NULL;
    return 1;
}

/*
 * function stringlist_print
 */
void
stringlist_print (stringlist ** lst)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function stringlist_print\n");
    }

    while (*lst)
    {
        printf ("%s %d %d\n", (*lst)->data, (*lst)->normalFID, (*lst)->accountFID);
        lst = &((*lst)->next);
    }
}

/*
 * function stringlist_delete
 */
stringlist *
stringlist_delete (stringlist ** lst)
{
    stringlist *first;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function stringlist_delete\n");
    }

    if (*lst)
    {
        first = (*lst)->next;
        free ((*lst)->data);
        free (*lst);
        return (first);
    }
    else
    {
        return (NULL);
    }
}

/*
 * function stringlist_search
 */
stringlist *
stringlist_search (stringlist ** lst, char *searchstring, char **result)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function stringlist_search\n");
    }

    /*
     * return if searchstring is NULL
     */
    if (searchstring == NULL)
    {
        return NULL;
    }

    /*
     * compare all elements of list with given string
     */
    while (*lst)
    {
        if (strstr ((*lst)->data, searchstring))
        {
            *result = (*lst)->data;
//                      *result = string_duplicate((*lst)->data);
            return (*lst);
        }
        lst = &((*lst)->next);
    }
    return NULL;
}

/*
 * function create_fw1_filter_rule
 */
LeaFilterRulebase *
create_fw1_filter_rule (LeaFilterRulebase * prulebase, char filterstring[255])
{
    LeaFilterRule *prule;
    LeaFilterPredicate *ppred;
    char *filterargument;
    char *argumentvalue;
    char *argumentname;
    unsigned int tempint;
    unsigned int tmpint1;
    unsigned int tmpint2;
    unsigned short tempushort;
    unsigned short tmpushort1;
    unsigned short tmpushort2;
    unsigned long templong;
    lea_value_ex_t **val_arr;
    lea_value_ex_t *lea_value;
    char *argumentsinglevalue;
    int argumentcount;
    int negation;
    char *tmpstring1;
    char *tmpstring2;
    struct tm timestruct;
    char tempchararray[10];

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function create_fw1_filter_rule\n");
    }

    /*
     * create an empty rule with action "pass"
     */
    if ((prule = lea_filter_rule_create (LEA_FILTER_ACTION_PASS)) == NULL)
    {
        fprintf (stderr, "ERROR: failed to create rule\n");
        lea_filter_rulebase_destroy (prulebase);
        return NULL;
    }

    /*
     * split filter string in arguments separated by ";"
     */
    filterargument = strtok (filterstring, ";");
    while (filterargument != NULL)
    {
        /*
         * split argument into name and value separated by "="
         */
        argumentvalue = strchr (filterargument, '=');
        if (argumentvalue == NULL)
        {
            fprintf (stderr, "ERROR: syntax error in rule argument '%s'.\n"
                     "       Required syntax: 'argument=value'\n",
                     filterargument);
            return NULL;
        }
        argumentvalue++;
        argumentname = filterargument;
        argumentname[argumentvalue - filterargument - 1] = '\0';
        argumentvalue = string_trim (argumentvalue, ' ');
        argumentname = string_trim (argumentname, ' ');
        filterargument = strtok (NULL, ";");
        val_arr = NULL;

        if (argumentname[strlen (argumentname) - 1] == '!')
        {
            negation = 1;
            argumentname = string_trim (argumentname, '!');
        }
        else
        {
            negation = 0;
        }

        /*
         * change argument name to lower case letters
         */
        for (tempint = 0; tempint < strlen (argumentname); tempint++)
        {
            argumentname[tempint] = tolower (argumentname[tempint]);
        }

        /*
         * process arguments of type "product"
         */
        if (strcmp (argumentname, "product") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * check validity of argument value
                 */
                if (!((strcmp (argumentsinglevalue, "VPN-1 & FireWall-1") == 0)
                         || (strcmp (argumentsinglevalue, "SmartDefense") == 0)))
                {
                    fprintf (stderr, "ERROR: invalid value for product: '%s'\n",
                             argumentsinglevalue);
                    return NULL;
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_STRING,
                         argumentsinglevalue) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr, "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("product", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        else if (strcmp (argumentname, "fw_subproduct") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_STRING,
                         argumentsinglevalue) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr, "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("fw_subproduct", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "action"
         */
        else if (strcmp (argumentname, "action") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * transform values (accept, drop, reject) into corresponding values
                 */
                if (strcmp (argumentsinglevalue, "ctl") == 0)
                {
                    tempint = 0;
                }
                else if (strcmp (argumentsinglevalue, "drop") == 0)
                {
                    tempint = 2;
                }
                else if (strcmp (argumentsinglevalue, "reject") == 0)
                {
                    tempint = 3;
                }
                else if (strcmp (argumentsinglevalue, "accept") == 0)
                {
                    tempint = 4;
                }
                else if (strcmp (argumentsinglevalue, "encrypt") == 0)
                {
                    tempint = 5;
                }
                else if (strcmp (argumentsinglevalue, "decrypt") == 0)
                {
                    tempint = 6;
                }
                else if (strcmp (argumentsinglevalue, "keyinst") == 0)
                {
                    tempint = 7;
                }
                else
                {
                    fprintf (stderr, "ERROR: invalid value for action: '%s'\n",
                             argumentsinglevalue);
                    return NULL;
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_ACTION,
                         tempint) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr, "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("action", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "orig"
         */
        else if (strcmp (argumentname, "orig") == 0)
        {
            /*
             * if the value specifies a network, create the filter predicate directly
             */
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                if (strlen (argumentsinglevalue) == 0)
                {
                    fprintf (stderr,
                             "ERROR: syntax error in rule value of argument orig: '%s'.\n"
                             "       Required syntax: 'orig=aaa.bbb.ccc.ddd'\n",
                             argumentsinglevalue);
                    return NULL;
                }
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_IP_ADDR,
                         inet_addr (argumentsinglevalue)) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr,
                             "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("orig", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "dst"
         */
        else if (strcmp (argumentname, "dst") == 0)
        {
            /*
             * check whether values are valid
             */
            if ((strchr (argumentvalue, ',')) && (strchr (argumentvalue, '/')))
            {
                fprintf (stderr,
                         "ERROR: use either netmask OR multiple IP addresses");
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            /*
             * if the value specifies a network, create the filter predicate directly
             */
            if (strchr (argumentvalue, '/'))
            {
                tmpstring1 =
                    string_trim (string_get_token (&argumentvalue, '/'), ' ');
                tmpstring2 =
                    string_trim (string_get_token (&argumentvalue, '/'), ' ');
                if ((strlen (tmpstring1) == 0) || (strlen (tmpstring2) == 0))
                {
                    fprintf (stderr,
                             "ERROR: syntax error in rule value of argument dst: '%s/%s'.\n"
                             "       Required syntax: 'dst=aaa.bbb.ccc.ddd/eee.fff.ggg.hhh'\n",
                             tmpstring1, tmpstring2);
                    return NULL;
                }
                if ((ppred =
                            lea_filter_predicate_create ("dst", -1, negation,
                                    LEA_FILTER_PRED_BELONGS_TO_MASK,
                                    inet_addr (tmpstring1),
                                    inet_addr (tmpstring2))) ==
                        NULL)
                {
                    fprintf (stderr, "ERROR: failed to create predicate\n");
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }
            else
            {
                argumentcount = 0;
                /*
                 * get argument values separated by ","
                 */
                while (argumentvalue)
                {
                    argumentsinglevalue =
                        string_trim (string_get_token (&argumentvalue, ','), ' ');
                    if (strlen (argumentsinglevalue) == 0)
                    {
                        fprintf (stderr,
                                 "ERROR: syntax error in rule value of argument dst: '%s'.\n"
                                 "       Required syntax: 'dst=aaa.bbb.ccc.ddd'\n",
                                 argumentsinglevalue);
                        return NULL;
                    }
                    argumentcount++;
                    if (val_arr)
                    {
                        val_arr =
                            (lea_value_ex_t **) realloc (val_arr,
                                                         argumentcount *
                                                         sizeof (lea_value_ex_t
                                                                 *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }
                    else
                    {
                        val_arr =
                            (lea_value_ex_t **) malloc (argumentcount *
                                                        sizeof (lea_value_ex_t
                                                                *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }

                    /*
                     * create extended opsec value
                     */
                    val_arr[argumentcount - 1] = lea_value_ex_create ();
                    if (lea_value_ex_set
                            (val_arr[argumentcount - 1], LEA_VT_IP_ADDR,
                             inet_addr (argumentsinglevalue)) == OPSEC_SESSION_ERR)
                    {
                        fprintf (stderr,
                                 "ERROR: failed to set rule value (%s)\n",
                                 opsec_errno_str (opsec_errno));
                        lea_value_ex_destroy (val_arr[argumentcount - 1]);
                        lea_filter_rule_destroy (prule);
                        return NULL;
                    }
                }

                /*
                 * create filter predicate
                 */
                if ((ppred =
                            lea_filter_predicate_create ("dst", -1, negation,
                                    LEA_FILTER_PRED_BELONGS_TO,
                                    argumentcount,
                                    val_arr)) == NULL)
                {
                    fprintf (stderr, "ERROR: failed to create predicate\n");
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }

                lea_value_ex_destroy (val_arr[argumentcount - 1]);
            }

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "proto"
         */
        else if (strcmp (argumentname, "proto") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * transform values (icmp, tcp, udp) into corresponding values
                 */
                if (strcmp (argumentsinglevalue, "icmp") == 0)
                {
                    tempint = 1;
                }
                else if (strcmp (argumentsinglevalue, "tcp") == 0)
                {
                    tempint = 6;
                }
                else if (strcmp (argumentsinglevalue, "udp") == 0)
                {
                    tempint = 17;
                }
                else
                {
                    fprintf (stderr, "ERROR: invalid value for action: '%s'\n",
                             argumentsinglevalue);
                    return NULL;
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_IP_PROTO,
                         tempint) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr, "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("proto", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "starttime"
         */
        else if (strcmp (argumentname, "starttime") == 0)
        {
            argumentsinglevalue = string_trim (argumentvalue, ' ');
            if (strlen (argumentsinglevalue) != 14)
            {
                fprintf (stderr,
                         "ERROR: syntax error in rule value of argument rule: '%s'.\n"
                         "       Required syntax: 'starttime=YYYYMMDDhhmmss'\n",
                         argumentsinglevalue);
                return NULL;
            }

            /*
             * convert starttime parameter to proper form (unixtime)
             */
            strncpy (tempchararray, argumentsinglevalue, 4);
            tempchararray[4] = '\0';
            timestruct.tm_year =
                strtol (tempchararray, (char **) NULL, 10) - 1900;
            argumentsinglevalue += 4 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mon = strtol (tempchararray, (char **) NULL, 10) - 1;
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mday = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_hour = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_min = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_sec = strtol (tempchararray, (char **) NULL, 10);

            /*
             * convert starttime parameter to long int
             */
            if ((timestruct.tm_mon > 11) || (timestruct.tm_mday > 31)
                    || (timestruct.tm_hour > 23) || (timestruct.tm_min > 59)
                    || (timestruct.tm_sec > 59))
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }
            templong = (unsigned long) mktime (&timestruct);
            if (templong == -1)
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }

            /*
             * create extended opsec value
             */
            lea_value = lea_value_ex_create ();
            if (lea_value_ex_set (lea_value, LEA_VT_TIME, templong) ==
                    OPSEC_SESSION_ERR)
            {
                fprintf (stderr, "ERROR: failed to set starttime value (%s)\n",
                         opsec_errno_str (opsec_errno));
                lea_value_ex_destroy (lea_value);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("time", -1, negation,
                                LEA_FILTER_PRED_GREATER_EQUAL,
                                lea_value)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (lea_value);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "endtime"
         */
        else if (strcmp (argumentname, "endtime") == 0)
        {
            argumentsinglevalue = string_trim (argumentvalue, ' ');
            if (strlen (argumentsinglevalue) != 14)
            {
                fprintf (stderr,
                         "ERROR: syntax error in rule value of argument rule: '%s'.\n"
                         "       Required syntax: 'endtime=YYYYMMDDhhmmss'\n",
                         argumentsinglevalue);
                return NULL;
            }

            /*
             * convert starttime parameter to proper form (unixtime)
             */
            strncpy (tempchararray, argumentsinglevalue, 4);
            tempchararray[4] = '\0';
            timestruct.tm_year =
                strtol (tempchararray, (char **) NULL, 10) - 1900;
            argumentsinglevalue += 4 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mon = strtol (tempchararray, (char **) NULL, 10) - 1;
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mday = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_hour = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_min = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_sec = strtol (tempchararray, (char **) NULL, 10);

            /*
             * convert starttime parameter to long int
             */
            if ((timestruct.tm_mon > 11) || (timestruct.tm_mday > 31)
                    || (timestruct.tm_hour > 23) || (timestruct.tm_min > 59)
                    || (timestruct.tm_sec > 59))
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }
            templong = (unsigned long) mktime (&timestruct);
            if (templong == -1)
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }

            /*
             * create extended opsec value
             */
            lea_value = lea_value_ex_create ();
            if (lea_value_ex_set (lea_value, LEA_VT_TIME, templong) ==
                    OPSEC_SESSION_ERR)
            {
                fprintf (stderr, "ERROR: failed to set endtime value (%s)\n",
                         opsec_errno_str (opsec_errno));
                lea_value_ex_destroy (lea_value);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("time", -1, negation,
                                LEA_FILTER_PRED_SMALLER_EQUAL,
                                lea_value)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (lea_value);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "rule"
         */
        else if (strcmp (argumentname, "rule") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                if (strlen (argumentsinglevalue) == 0)
                {
                    fprintf (stderr,
                             "ERROR: syntax error in rule value of argument rule: '%s'.\n"
                             "       Required syntax: 'rule=x'\n",
                             argumentsinglevalue);
                    return NULL;
                }

                /*
                 * get ranges separated by "-", if there is no "-" start and end value of the
                 * range is the same.
                 */
                tmpstring1 =
                    string_trim (string_get_token (&argumentsinglevalue, '-'),
                                 ' ');
                tmpstring2 =
                    (argumentsinglevalue ==
                     NULL) ? tmpstring1 :
                    string_trim (string_get_token (&argumentsinglevalue, '-'),
                                 ' ');
                tmpint1 = (int) strtol (tmpstring1, (char **) NULL, 10);
                tmpint2 = (int) strtol (tmpstring2, (char **) NULL, 10);

                for (tempint = tmpint1; tempint <= tmpint2; tempint++)
                {
                    argumentcount++;
                    if (val_arr)
                    {
                        val_arr =
                            (lea_value_ex_t **) realloc (val_arr,
                                                         argumentcount *
                                                         sizeof (lea_value_ex_t
                                                                 *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }
                    else
                    {
                        val_arr =
                            (lea_value_ex_t **) malloc (argumentcount *
                                                        sizeof (lea_value_ex_t
                                                                *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }

                    /*
                     * create extended opsec value
                     */
                    val_arr[argumentcount - 1] = lea_value_ex_create ();
                    if (lea_value_ex_set
                            (val_arr[argumentcount - 1], LEA_VT_RULE,
                             tempint) == OPSEC_SESSION_ERR)
                    {
                        fprintf (stderr,
                                 "ERROR: failed to set rule value (%s)\n",
                                 opsec_errno_str (opsec_errno));
                        lea_value_ex_destroy (val_arr[argumentcount - 1]);
                        lea_filter_rule_destroy (prule);
                        return NULL;
                    }
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("rule", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "service"
         */
        else if (strcmp (argumentname, "service") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                if (strlen (argumentsinglevalue) == 0)
                {
                    fprintf (stderr,
                             "ERROR: syntax error in rule value of argument service: '%s'.\n"
                             "       Required syntax: 'service=<Port-Number>'\n",
                             argumentsinglevalue);
                    return NULL;
                }

                /*
                 * get ranges separated by "-", if there is no "-" start and end value of the
                 * range is the same.
                 */
                tmpstring1 =
                    string_trim (string_get_token (&argumentsinglevalue, '-'),
                                 ' ');
                tmpstring2 =
                    (argumentsinglevalue ==
                     NULL) ? tmpstring1 :
                    string_trim (string_get_token (&argumentsinglevalue, '-'),
                                 ' ');
                tmpushort1 =
                    (unsigned short) strtol (tmpstring1, (char **) NULL, 10);
                tmpushort2 =
                    (unsigned short) strtol (tmpstring2, (char **) NULL, 10);

                for (tempushort = tmpushort1; tempushort <= tmpushort2;
                        tempushort++)
                {
                    argumentcount++;
                    if (val_arr)
                    {
                        val_arr =
                            (lea_value_ex_t **) realloc (val_arr,
                                                         argumentcount *
                                                         sizeof (lea_value_ex_t
                                                                 *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }
                    else
                    {
                        val_arr =
                            (lea_value_ex_t **) malloc (argumentcount *
                                                        sizeof (lea_value_ex_t
                                                                *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }

                    /*
                     * create extended opsec value
                     */
                    val_arr[argumentcount - 1] = lea_value_ex_create ();
                    if (lea_value_ex_set
                            (val_arr[argumentcount - 1], LEA_VT_USHORT,
                             tempushort) == OPSEC_SESSION_ERR)
                    {
                        fprintf (stderr,
                                 "ERROR: failed to set rule value (%s)\n",
                                 opsec_errno_str (opsec_errno));
                        lea_value_ex_destroy (val_arr[argumentcount - 1]);
                        lea_filter_rule_destroy (prule);
                        return NULL;
                    }
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("service", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "src"
         */
        else if (strcmp (argumentname, "src") == 0)
        {
            /*
             * check whether values are valid
             */
            if ((strchr (argumentvalue, ',')) && (strchr (argumentvalue, '/')))
            {
                fprintf (stderr,
                         "ERROR: use either netmask OR multiple IP addresses");
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            /*
             * if the value specifies a network, create the filter predicate directly
             */
            if (strchr (argumentvalue, '/'))
            {
                tmpstring1 =
                    string_trim (string_get_token (&argumentvalue, '/'), ' ');
                tmpstring2 =
                    string_trim (string_get_token (&argumentvalue, '/'), ' ');
                if ((strlen (tmpstring1) == 0) || (strlen (tmpstring2) == 0))
                {
                    fprintf (stderr,
                             "ERROR: syntax error in rule value of argument src: '%s/%s'.\n"
                             "       Required syntax: 'src=aaa.bbb.ccc.ddd/eee.fff.ggg.hhh'\n",
                             tmpstring1, tmpstring2);
                    return NULL;
                }
                if ((ppred =
                            lea_filter_predicate_create ("src", -1, negation,
                                    LEA_FILTER_PRED_BELONGS_TO_MASK,
                                    inet_addr (tmpstring1),
                                    inet_addr (tmpstring2))) ==
                        NULL)
                {
                    fprintf (stderr, "ERROR: failed to create predicate\n");
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }
            else
            {
                argumentcount = 0;
                /*
                 * get argument values separated by ","
                 */
                while (argumentvalue)
                {
                    argumentsinglevalue =
                        string_trim (string_get_token (&argumentvalue, ','), ' ');
                    if (strlen (argumentsinglevalue) == 0)
                    {
                        fprintf (stderr,
                                 "ERROR: syntax error in rule value of argument src: '%s'.\n"
                                 "       Required syntax: 'src=aaa.bbb.ccc.ddd'\n",
                                 argumentsinglevalue);
                        return NULL;
                    }
                    argumentcount++;
                    if (val_arr)
                    {
                        val_arr =
                            (lea_value_ex_t **) realloc (val_arr,
                                                         argumentcount *
                                                         sizeof (lea_value_ex_t
                                                                 *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }
                    else
                    {
                        val_arr =
                            (lea_value_ex_t **) malloc (argumentcount *
                                                        sizeof (lea_value_ex_t
                                                                *));
                        if (val_arr == NULL)
                        {
                            fprintf (stderr, "ERROR: Out of memory\n");
                            exit_loggrabber (1);
                        }
                    }

                    /*
                     * create extended opsec value
                     */
                    val_arr[argumentcount - 1] = lea_value_ex_create ();
                    if (lea_value_ex_set
                            (val_arr[argumentcount - 1], LEA_VT_IP_ADDR,
                             inet_addr (argumentsinglevalue)) == OPSEC_SESSION_ERR)
                    {
                        fprintf (stderr,
                                 "ERROR: failed to set rule value (%s)\n",
                                 opsec_errno_str (opsec_errno));
                        lea_value_ex_destroy (val_arr[argumentcount - 1]);
                        lea_filter_rule_destroy (prule);
                        return NULL;
                    }
                }

                /*
                 * create filter predicate
                 */
                if ((ppred =
                            lea_filter_predicate_create ("src", -1, negation,
                                    LEA_FILTER_PRED_BELONGS_TO,
                                    argumentcount,
                                    val_arr)) == NULL)
                {
                    fprintf (stderr, "ERROR: failed to create predicate\n");
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }

                lea_value_ex_destroy (val_arr[argumentcount - 1]);
            }

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process unknown arguments
         */
        else
        {
            fprintf (stderr, "ERROR: Unknown filterargument: '%s'\n",
                     argumentname);
            return NULL;
        }
    }

    /*
     * add current rule to rulebase
     */
    if (lea_filter_rulebase_add_rule (prulebase, prule) != OPSEC_SESSION_OK)
    {
        fprintf (stderr, "failed to add rule to rulebase\n");
        lea_filter_rulebase_destroy (prulebase);
        lea_filter_rule_destroy (prule);
        return NULL;
    }

    lea_filter_rule_destroy (prule);

    return prulebase;
}

/*
 * function create_audit_filter_rule
 */
LeaFilterRulebase *
create_audit_filter_rule (LeaFilterRulebase * prulebase,
                          char filterstring[255])
{
    LeaFilterRule *prule;
    LeaFilterPredicate *ppred;
    char *filterargument;
    char *argumentvalue;
    char *argumentname;
    unsigned int tempint;
    unsigned long templong;
    lea_value_ex_t **val_arr;
    lea_value_ex_t *lea_value;
    char *argumentsinglevalue;
    int argumentcount;
    int negation;
    struct tm timestruct;
    char tempchararray[10];

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function create_audit_filter_rule\n");
    }

    /*
     * create an empty rule with action "pass"
     */
    if ((prule = lea_filter_rule_create (LEA_FILTER_ACTION_PASS)) == NULL)
    {
        fprintf (stderr, "ERROR: failed to create rule\n");
        lea_filter_rulebase_destroy (prulebase);
        return NULL;
    }

    /*
     * split filter string in arguments separated by ";"
     */
    filterargument = strtok (filterstring, ";");
    while (filterargument != NULL)
    {
        /*
         * split argument into name and value separated by "="
         */
        argumentvalue = strchr (filterargument, '=');
        if (argumentvalue == NULL)
        {
            fprintf (stderr, "ERROR: syntax error in rule argument '%s'.\n"
                     "       Required syntax: 'argument=value'\n",
                     filterargument);
            return NULL;
        }
        argumentvalue++;
        argumentname = filterargument;
        argumentname[argumentvalue - filterargument - 1] = '\0';
        argumentvalue = string_trim (argumentvalue, ' ');
        argumentname = string_trim (argumentname, ' ');
        filterargument = strtok (NULL, ";");
        val_arr = NULL;

        if (argumentname[strlen (argumentname) - 1] == '!')
        {
            negation = 1;
            argumentname = string_trim (argumentname, '!');
        }
        else
        {
            negation = 0;
        }

        /*
         * change argument name to lower case letters
         */
        for (tempint = 0; tempint < strlen (argumentname); tempint++)
        {
            argumentname[tempint] = tolower (argumentname[tempint]);
        }

        /*
         * process arguments of type "product"
         */
        if (strcmp (argumentname, "product") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * check validity of argument value
                 */
                if (!((strcmp (argumentsinglevalue, "SmartDashboard") == 0) ||
                        (strcmp (argumentsinglevalue, "Policy Editor") == 0) ||
                        (strcmp (argumentsinglevalue, "SmartView Tracker") == 0)
                        || (strcmp (argumentsinglevalue, "SmartView Status") == 0)
                        || (strcmp (argumentsinglevalue, "SmartView Monitor") ==
                            0)
                        || (strcmp (argumentsinglevalue, "System Monitor") == 0)
                        || (strcmp (argumentsinglevalue, "cpstat_monitor") == 0)
                        || (strcmp (argumentsinglevalue, "SmartUpdate") == 0)
                        || (strcmp (argumentsinglevalue, "CPMI Client") == 0)))
                {
                    fprintf (stderr, "ERROR: invalid value for product: '%s'\n",
                             argumentsinglevalue);
                    return NULL;
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_STRING,
                         argumentsinglevalue) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr, "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("product", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "Administrator"
         */
        else if (strcmp (argumentname, "administrator") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_STRING,
                         argumentsinglevalue) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr, "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("Administrator", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "orig"
         */
        else if (strcmp (argumentname, "orig") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                if (strlen (argumentsinglevalue) == 0)
                {
                    fprintf (stderr,
                             "ERROR: syntax error in rule value of argument orig: '%s'.\n"
                             "       Required syntax: 'orig=aaa.bbb.ccc.ddd'\n",
                             argumentsinglevalue);
                    return NULL;
                }
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_IP_ADDR,
                         inet_addr (argumentsinglevalue)) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr,
                             "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("orig", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "action"
         */
        else if (strcmp (argumentname, "action") == 0)
        {
            argumentcount = 0;
            /*
             * get argument values separated by ","
             */
            while (argumentvalue)
            {
                argumentsinglevalue =
                    string_trim (string_get_token (&argumentvalue, ','), ' ');
                argumentcount++;
                if (val_arr)
                {
                    val_arr =
                        (lea_value_ex_t **) realloc (val_arr,
                                                     argumentcount *
                                                     sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }
                else
                {
                    val_arr =
                        (lea_value_ex_t **) malloc (argumentcount *
                                                    sizeof (lea_value_ex_t *));
                    if (val_arr == NULL)
                    {
                        fprintf (stderr, "ERROR: Out of memory\n");
                        exit_loggrabber (1);
                    }
                }

                /*
                 * transform values (accept, drop, reject) into corresponding values
                 */
                if (strcmp (argumentsinglevalue, "ctl") == 0)
                {
                    tempint = 0;
                }
                else if (strcmp (argumentsinglevalue, "drop") == 0)
                {
                    tempint = 2;
                }
                else if (strcmp (argumentsinglevalue, "reject") == 0)
                {
                    tempint = 3;
                }
                else if (strcmp (argumentsinglevalue, "accept") == 0)
                {
                    tempint = 4;
                }
                else if (strcmp (argumentsinglevalue, "encrypt") == 0)
                {
                    tempint = 5;
                }
                else if (strcmp (argumentsinglevalue, "decrypt") == 0)
                {
                    tempint = 6;
                }
                else if (strcmp (argumentsinglevalue, "keyinst") == 0)
                {
                    tempint = 7;
                }
                else
                {
                    fprintf (stderr, "ERROR: invalid value for action: '%s'\n",
                             argumentsinglevalue);
                    return NULL;
                }

                /*
                 * create extended opsec value
                 */
                val_arr[argumentcount - 1] = lea_value_ex_create ();
                if (lea_value_ex_set
                        (val_arr[argumentcount - 1], LEA_VT_ACTION,
                         tempint) == OPSEC_SESSION_ERR)
                {
                    fprintf (stderr, "ERROR: failed to set rule value (%s)\n",
                             opsec_errno_str (opsec_errno));
                    lea_value_ex_destroy (val_arr[argumentcount - 1]);
                    lea_filter_rule_destroy (prule);
                    return NULL;
                }
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("action", -1, negation,
                                LEA_FILTER_PRED_BELONGS_TO,
                                argumentcount, val_arr)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (val_arr[argumentcount - 1]);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "starttime"
         */
        else if (strcmp (argumentname, "starttime") == 0)
        {
            argumentsinglevalue = string_trim (argumentvalue, ' ');
            if (strlen (argumentsinglevalue) != 14)
            {
                fprintf (stderr,
                         "ERROR: syntax error in rule value of argument rule: '%s'.\n"
                         "       Required syntax: 'starttime=YYYYMMDDhhmmss'\n",
                         argumentsinglevalue);
                return NULL;
            }

            /*
             * convert starttime parameter to proper form (unixtime)
             */
            strncpy (tempchararray, argumentsinglevalue, 4);
            tempchararray[4] = '\0';
            timestruct.tm_year =
                strtol (tempchararray, (char **) NULL, 10) - 1900;
            argumentsinglevalue += 4 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mon = strtol (tempchararray, (char **) NULL, 10) - 1;
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mday = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_hour = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_min = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_sec = strtol (tempchararray, (char **) NULL, 10);

            /*
             * convert starttime parameter to long int
             */
            if ((timestruct.tm_mon > 11) || (timestruct.tm_mday > 31)
                    || (timestruct.tm_hour > 23) || (timestruct.tm_min > 59)
                    || (timestruct.tm_sec > 59))
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }
            templong = (unsigned long) mktime (&timestruct);
            if (templong == -1)
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }

            /*
             * create extended opsec value
             */
            lea_value = lea_value_ex_create ();
            if (lea_value_ex_set (lea_value, LEA_VT_TIME, templong) ==
                    OPSEC_SESSION_ERR)
            {
                fprintf (stderr, "ERROR: failed to set starttime value (%s)\n",
                         opsec_errno_str (opsec_errno));
                lea_value_ex_destroy (lea_value);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("time", -1, negation,
                                LEA_FILTER_PRED_GREATER_EQUAL,
                                lea_value)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (lea_value);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process arguments of type "endtime"
         */
        else if (strcmp (argumentname, "endtime") == 0)
        {
            argumentsinglevalue = string_trim (argumentvalue, ' ');
            if (strlen (argumentsinglevalue) != 14)
            {
                fprintf (stderr,
                         "ERROR: syntax error in rule value of argument rule: '%s'.\n"
                         "       Required syntax: 'endtime=YYYYMMDDhhmmss'\n",
                         argumentsinglevalue);
                return NULL;
            }

            /*
             * convert starttime parameter to proper form (unixtime)
             */
            strncpy (tempchararray, argumentsinglevalue, 4);
            tempchararray[4] = '\0';
            timestruct.tm_year =
                strtol (tempchararray, (char **) NULL, 10) - 1900;
            argumentsinglevalue += 4 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mon = strtol (tempchararray, (char **) NULL, 10) - 1;
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_mday = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_hour = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_min = strtol (tempchararray, (char **) NULL, 10);
            argumentsinglevalue += 2 * sizeof (char);
            strncpy (tempchararray, argumentsinglevalue, 2);
            tempchararray[2] = '\0';
            timestruct.tm_sec = strtol (tempchararray, (char **) NULL, 10);

            /*
             * convert starttime parameter to long int
             */
            if ((timestruct.tm_mon > 11) || (timestruct.tm_mday > 31)
                    || (timestruct.tm_hour > 23) || (timestruct.tm_min > 59)
                    || (timestruct.tm_sec > 59))
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }
            templong = (unsigned long) mktime (&timestruct);
            if (templong == -1)
            {
                fprintf (stderr,
                         "ERROR: illegal date format in argumentvalue\n");
                return NULL;
            }

            /*
             * create extended opsec value
             */
            lea_value = lea_value_ex_create ();
            if (lea_value_ex_set (lea_value, LEA_VT_TIME, templong) ==
                    OPSEC_SESSION_ERR)
            {
                fprintf (stderr, "ERROR: failed to set endtime value (%s)\n",
                         opsec_errno_str (opsec_errno));
                lea_value_ex_destroy (lea_value);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            /*
             * create filter predicate
             */
            if ((ppred =
                        lea_filter_predicate_create ("time", -1, negation,
                                LEA_FILTER_PRED_SMALLER_EQUAL,
                                lea_value)) == NULL)
            {
                fprintf (stderr, "ERROR: failed to create predicate\n");
                lea_value_ex_destroy (val_arr[argumentcount - 1]);
                lea_filter_rule_destroy (prule);
                return NULL;
            }

            lea_value_ex_destroy (lea_value);

            /*
             * add current predicate to current rule
             */
            if (lea_filter_rule_add_predicate (prule, ppred) == LEA_FILTER_ERR)
            {
                fprintf (stderr, "ERROR: failed to add predicate to rule\n");
                lea_filter_rule_destroy (prule);
                lea_filter_predicate_destroy (ppred);
                return NULL;
            }

            lea_filter_predicate_destroy (ppred);
        }

        /*
         * process unknown arguments
         */
        else
        {
            fprintf (stderr, "ERROR: Unknown filterargument: '%s'\n",
                     argumentname);
            return NULL;
        }
    }

    /*
     * add current rule to rulebase
     */
    if (lea_filter_rulebase_add_rule (prulebase, prule) != OPSEC_SESSION_OK)
    {
        fprintf (stderr, "failed to add rule to rulebase\n");
        lea_filter_rulebase_destroy (prulebase);
        lea_filter_rule_destroy (prule);
        return NULL;
    }

    lea_filter_rule_destroy (prule);

    return prulebase;
}

/*
 * BEGIN: function string_get_token
 */
char *
string_get_token (char **tokstring, char separator)
{
    char *tempstring = NULL;
    char *returnstring;
    int strlength;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_get_token\n");
    }

    /*
     * return if tokstring is NULL
     */
    if (*tokstring == NULL)
    {
        return NULL;
    }

    /*
     * search for first separator
     */
    tempstring = strchr (*tokstring, separator);

    /*
     * calculate string length
     */
    if (tempstring)
    {
        tempstring = tempstring + 1;
        strlength = strlen (*tokstring) - strlen (tempstring);
    }
    else
    {
        strlength = strlen (*tokstring) + 1;
    }

    returnstring = (char *) malloc (strlength + 1);
    if (returnstring == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }
    strncpy (returnstring, *tokstring, strlength);
    returnstring[strlength - 1] = '\0';

    *tokstring = tempstring;
    return returnstring;
}

/*
 * BEGIN: function string_duplicate
 */
char *
string_duplicate (const char *src)
{
    size_t length;
    char *dst;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_duplicate\n");
    }

    if (src == NULL)
    {
        return NULL;
    }

    length = strlen (src) + 1;
    dst = (char*) malloc (length);
    if (!dst)
    {
        fprintf (stderr, "ERROR: out of memory\n");
        exit_loggrabber (1);
    }
    return ((char*) memcpy (dst, src, length));
}

/*
 * BEGIN: function string_left_trim
 */
char *
string_left_trim (char *string, char character)
{
    char *tmp;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_left_trim\n");
    }

    if (string == NULL)
    {
        return NULL;
    }
    tmp = string + strlen (string);
    while ((string[0] == character) && (string < tmp))
        string++;
    return (string);
}

/*
 * BEGIN: function string_right_trim
 */
char *
string_right_trim (char *string, char character)
{
    int tmp;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_right_trim\n");
    }

    if (string == NULL)
    {
        return NULL;
    }
    tmp = strlen (string);
    while ((string[tmp - 1] == character) && (tmp != 0))
        tmp--;
    string[tmp] = '\0';
    return (string);
}

/*
 * BEGIN: function string_trim
 */
char *
string_trim (char *string, char character)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_trim\n");
    }

    return (string_right_trim
            (string_left_trim (string, character), character));
}

/*
 * BEGIN: function string_mask_newlines
 */
char *
string_mask_newlines (char *string)
{
    int i = 0;
    int z1, z2;
    int length = strlen (string);
    char *sptr = string;
    char *s;

    sptr = strchr (sptr, '\n');
    while (sptr != NULL)
    {
        i++;
        sptr = strchr (++sptr, '\n');
    }

    s = (char *) malloc (length + (i * 2) + 1);

    if (!s)
    {
        fprintf (stderr, "ERROR: out of memory\n");
        exit_loggrabber (1);
    }

    for (z1 = 0, z2 = 0; z1 < length; z1++)
    {
        if (string[z1] == '\n')
        {
            s[z2++] = '(';
            s[z2++] = '+';
            s[z2++] = ')';
        }
        else
        {
            s[z2++] = string[z1];
        }
    }

    s[z2] = '\0';

    return (s);
}

/*
 * BEGIN: function string_escape
 */
char *
string_escape (const char *string, char character)
{
    int i = strlen (string);
    int z1, z2;
    char *s = (char *) malloc (i * 2 + 1);

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_escape\n");
    }

    /*
     * return if string is NULL
     */
    if (string == NULL)
    {
        return NULL;
    }

    if (!s)
    {
        fprintf (stderr, "ERROR: out of memory\n");
        exit_loggrabber (1);
    }

    for (z1 = 0, z2 = 0; z1 < i; z1++)
    {
        if ((string[z1] == character) || (string[z1] == '\\'))
        {
            s[z2++] = '\\';
        }
        s[z2++] = string[z1];
    }

    s[z2] = '\0';

    return (s);
}

/*
 * BEGIN: function string_rmchar
 */
char *
string_rmchar (char *string, char character)
{
    int i = strlen (string);
    int z1, z2;
    char *s = (char *) malloc (i + 1);

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_rmchar\n");
    }

    /*
     * return if string is NULL
     */
    if (string == NULL)
    {
        return NULL;
    }

    if (!s)
    {
        fprintf (stderr, "ERROR: out of memory\n");
        exit_loggrabber (1);
    }

    for (z1 = 0, z2 = 0; z1 < i; z1++)
    {
        if (string[z1] != character)
        {
            s[z2++] = string[z1];
        }
    }

    s[z2] = '\0';

    return (s);
}

/*
 * BEGIN: function string_touppper
 */
char *
string_toupper (const char *str1)
{
    char *tempstr1;
    unsigned int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_toupper\n");
    }

    /*
     * return if string is NULL
     */
    if (str1 == NULL)
    {
        return NULL;
    }

    tempstr1 = string_duplicate (str1);

    for (i = 0; i < strlen (tempstr1); i++)
    {
        tempstr1[i] = toupper (tempstr1[i]);
    }

    return (tempstr1);
}

/*
 * BEGIN: function string_icmp
 */
int
string_icmp (const char *str1, const char *str2)
{
    char *tempstr1;
    char *tempstr2;
    int cmpresult;
    unsigned int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_icmp\n");
    }

    tempstr1 = string_duplicate (str1);
    tempstr2 = string_duplicate (str2);

    for (i = 0; i < strlen (tempstr1); i++)
    {
        tempstr1[i] = tolower (tempstr1[i]);
    }

    for (i = 0; i < strlen (tempstr2); i++)
    {
        tempstr2[i] = tolower (tempstr2[i]);
    }

    cmpresult = strcmp (tempstr1, tempstr2);

    free (tempstr1);
    free (tempstr2);

    return (cmpresult);
}

/*
 * BEGIN: function string_incmp
 */
int
string_incmp (const char *str1, const char *str2, size_t count)
{
    char *tempstr1;
    char *tempstr2;
    int cmpresult;
    unsigned int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function string_incmp\n");
    }

    tempstr1 = string_duplicate (str1);
    tempstr2 = string_duplicate (str2);

    for (i = 0; i < strlen (tempstr1); i++)
    {
        tempstr1[i] = tolower (tempstr1[i]);
    }

    for (i = 0; i < strlen (tempstr2); i++)
    {
        tempstr2[i] = tolower (tempstr2[i]);
    }

    cmpresult = strncmp (tempstr1, tempstr2, count);

    free (tempstr1);
    free (tempstr2);

    return (cmpresult);
}

/*
 * BEGIN: function to read configuration file
 */
void
read_config_file (char *filename, configvalues * cfgvalues)
{
    FILE *configfile;
    char line[256];
    char *position;
    char *configparameter;
    char *configvalue;
    char *tmpstr;

    if ((configfile = fopen (filename, "r")) == NULL)
    {
        fprintf (stderr, "ERROR: Cannot open configfile (%s)\n", filename);
        exit_loggrabber (1);
    }

    while (fgets (line, sizeof line, configfile))
    {
        position = strchr (line, '\n');
        if (position)
        {
            *position = 0;
        }

        position = strchr (line, '#');
        if (position)
        {
            *position = 0;
        }

        configparameter = string_trim (strtok (line, "="), ' ');
        if (configparameter)
        {
            configvalue = string_trim (strtok (NULL, ""), ' ');
        }

        if (configparameter && configvalue)
        {
            if (debug_mode == 1)
            {
                fprintf (stderr, "DEBUG: %s=%s\n", configparameter,
                         configvalue);
            }
            if (strcmp (configparameter, "RECORD_SEPARATOR") == 0)
            {
                tmpstr = string_trim (configvalue, '"');
                if (tmpstr)
                {
                    cfgvalues->record_separator = tmpstr[0];
                }
            }
            else if (strcmp (configparameter, "DEBUG_LEVEL") == 0)
            {
                cfgvalues->debug_mode = atoi (string_trim (configvalue, '"'));
            }

            else if (strcmp (configparameter, "SPLUNK_REST_MAX_RETRIES") == 0)
            {
                cfgvalues->splunkRestMaxRetries = atoi (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "SPLUNK_REST_RETRY_FACTOR") == 0)
            {
                cfgvalues->splunkRestRetryFactor = atoi (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "SPLUNK_REST_STATUS_COMMIT") == 0)
            {
                cfgvalues->splunkRestStatusCommit = atoi (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "ONLINE_MODE") == 0)
            {
                configvalue = string_duplicate (string_trim (configvalue, '"'));
                if (string_icmp (configvalue, "no") == 0)
                {
                    cfgvalues->online_mode = 0;
                }
                else if (string_icmp (configvalue, "yes") == 0)
                {
                    cfgvalues->online_mode = 1;
                }
                else
                {
                    fprintf (stderr,
                             "WARNING: Illegal entry in configuration file: %s=%s\n",
                             configparameter, configvalue);
                }
                free (configvalue);
            }
            else if (strcmp (configparameter, "RESOLVE_MODE") == 0)
            {
                configvalue = string_duplicate (string_trim (configvalue, '"'));
                if (string_icmp (configvalue, "no") == 0)
                {
                    cfgvalues->resolve_mode = 0;
                }
                else if (string_icmp (configvalue, "yes") == 0)
                {
                    cfgvalues->resolve_mode = 1;
                }
                else
                {
                    fprintf (stderr,
                             "WARNING: Illegal entry in configuration file: %s=%s\n",
                             configparameter, configvalue);
                }
                free (configvalue);
            }
            else if (strcmp (configparameter, "FW1_TYPE") == 0)
            {
                configvalue = string_duplicate (string_trim (configvalue, '"'));
                if (string_icmp (configvalue, "ng") == 0)
                {
                    cfgvalues->fw1_2000 = 0;
                }
                else if (string_icmp (configvalue, "2000") == 0)
                {
                    cfgvalues->fw1_2000 = 1;
                }
                else
                {
                    fprintf (stderr,
                             "WARNING: Illegal entry in configuration file: %s=%s\n",
                             configparameter, configvalue);
                }
                free (configvalue);
            }
            else if (strcmp (configparameter, "FW1_MODE") == 0)
            {
                configvalue = string_duplicate (string_trim (configvalue, '"'));
                if (string_icmp (configvalue, "normal") == 0)
                {
                    cfgvalues->audit_mode = 0;
                }
                else if (string_icmp (configvalue, "audit") == 0)
                {
                    cfgvalues->audit_mode = 1;
                }
                else
                {
                    fprintf (stderr,
                             "WARNING: Illegal entry in configuration file: %s=%s\n",
                             configparameter, configvalue);
                }
                free (configvalue);
            }
            else if (strcmp (configparameter, "DATEFORMAT") == 0)
            {
                configvalue = string_duplicate (string_trim (configvalue, '"'));
                if (string_icmp (configvalue, "cp") == 0)
                {
                    cfgvalues->dateformat = DATETIME_CP;
                }
                else if (string_icmp (configvalue, "unix") == 0)
                {
                    cfgvalues->dateformat = DATETIME_UNIX;
                }
                else if (string_icmp (configvalue, "std") == 0)
                {
                    cfgvalues->dateformat = DATETIME_STD;
                }
                else
                {
                    fprintf (stderr,
                             "WARNING: Illegal entry in configuration file: %s=%s\n",
                             configparameter, configvalue);
                }
                free (configvalue);
            }
            else if (strcmp (configparameter, "LOGGING_CONFIGURATION") == 0)
            {
                configvalue = string_duplicate (string_trim (configvalue, '"'));
                if (string_icmp (configvalue, "screen") == 0)
                {
                    cfgvalues->log_mode = SCREEN;
                }
                else if (string_icmp (configvalue, "file") == 0)
                {
                    cfgvalues->log_mode = LOGFILE;
                }
                else
                {
                    fprintf (stderr,
                             "WARNING: Illegal entry in configuration file: %s=%s\n",
                             configparameter, configvalue);
                }
                free (configvalue);
            }
            else if (strcmp (configparameter, "OUTPUT_FILE_PREFIX") == 0)
            {
                cfgvalues->output_file_prefix =
                    string_duplicate (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "OUTPUT_FILE_ROTATESIZE") == 0)
            {
                cfgvalues->output_file_rotatesize =
                    atol (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "FW1_OUTPUT") == 0)
            {
                configvalue = string_duplicate (string_trim (configvalue, '"'));
                if (string_icmp (configvalue, "files") == 0)
                {
                    cfgvalues->showfiles_mode = 1;
                }
                else if (string_icmp (configvalue, "logs") == 0)
                {
                    cfgvalues->showfiles_mode = 0;
                }
                else
                {
                    fprintf (stderr,
                             "WARNING: Illegal entry in configuration file: %s=%s\n",
                             configparameter, configvalue);
                }
                free (configvalue);
            }
            else if (strcmp (configparameter, "FW1_LOGFILE") == 0)
            {
                cfgvalues->fw1_logfile =
                    string_duplicate (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "FIELDS") == 0)
            {
                if (cfgvalues->fields != NULL)
                {
                    fprintf (stderr,
                             "ERROR: multiple FIELDS definitions in configuration file: %s=%s\n",
                             configparameter, configvalue);
                    exit_loggrabber (1);
                }
                cfgvalues->fields =
                    string_duplicate (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "FW1_FILTER_RULE") == 0)
            {
                cfgvalues->fw1_filter_count++;
                cfgvalues->fw1_filter_array =
                    (char **) realloc (cfgvalues->fw1_filter_array,
                                       cfgvalues->fw1_filter_count *
                                       sizeof (char *));
                if (cfgvalues->fw1_filter_array == NULL)
                {
                    fprintf (stderr, "ERROR: Out of memory\n");
                    exit_loggrabber (1);
                }
                cfgvalues->fw1_filter_array[cfgvalues->fw1_filter_count - 1] =
                    string_duplicate (string_trim (configvalue, '"'));
            }
            else if (strcmp (configparameter, "AUDIT_FILTER_RULE") == 0)
            {
                cfgvalues->audit_filter_count++;
                cfgvalues->audit_filter_array =
                    (char **) realloc (cfgvalues->audit_filter_array,
                                       cfgvalues->audit_filter_count *
                                       sizeof (char *));
                if (cfgvalues->audit_filter_array == NULL)
                {
                    fprintf (stderr, "ERROR: Out of memory\n");
                    exit_loggrabber (1);
                }
                cfgvalues->audit_filter_array[cfgvalues->audit_filter_count -
                                              1] =
                                                  string_duplicate (string_trim (configvalue, '"'));
            }
            else
            {
                fprintf (stderr,
                         "WARNING: Illegal entry in configuration file: %s=%s\n",
                         configparameter, configvalue);
            }
        }
    }

    fclose (configfile);
}

/*
 * BEGIN: function to initialize fields headers of logfile fields
 */
void
initialize_lfield_headers (char **headers[NUMBER_LIDX_FIELDS])
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_lfield_headers\n");
    }

    for (i = 0; i < NUMBER_LIDX_FIELDS; i++)
    {
        headers[i] = (char**) malloc (sizeof (char *));
        *headers[i] = NULL;
    }

    *headers[LIDX_NUM] = string_duplicate ("loc");
    *headers[LIDX_TIME] = string_duplicate ("time");
    *headers[LIDX_ACTION] = string_duplicate ("action");
    *headers[LIDX_ORIG] = string_duplicate ("orig");
    *headers[LIDX_ALERT] = string_duplicate ("alert");
    *headers[LIDX_IF_DIR] = string_duplicate ("i/f_dir");
    *headers[LIDX_IF_NAME] = string_duplicate ("i/f_name");
    *headers[LIDX_HAS_ACCOUNTING] = string_duplicate ("has_accounting");
    *headers[LIDX_UUID] = string_duplicate ("uuid");
    *headers[LIDX_PRODUCT] = string_duplicate ("product");
    *headers[LIDX_POLICY_ID_TAG] = string_duplicate ("__policy_id_tag");
    *headers[LIDX_SRC] = string_duplicate ("src");
    *headers[LIDX_S_PORT] = string_duplicate ("s_port");
    *headers[LIDX_DST] = string_duplicate ("dst");
    *headers[LIDX_SERVICE] = string_duplicate ("service");
    *headers[LIDX_TCP_FLAGS] = string_duplicate ("tcp_flags");
    *headers[LIDX_PROTO] = string_duplicate ("proto");
    *headers[LIDX_RULE] = string_duplicate ("rule");
    *headers[LIDX_XLATESRC] = string_duplicate ("xlatesrc");
    *headers[LIDX_XLATEDST] = string_duplicate ("xlatedst");
    *headers[LIDX_XLATESPORT] = string_duplicate ("xlatesport");
    *headers[LIDX_XLATEDPORT] = string_duplicate ("xlatedport");
    *headers[LIDX_NAT_RULENUM] = string_duplicate ("NAT_rulenum");
    *headers[LIDX_NAT_ADDRULENUM] = string_duplicate ("NAT_addtnl_rulenum");
    *headers[LIDX_RESOURCE] = string_duplicate ("resource");
    *headers[LIDX_ELAPSED] = string_duplicate ("elapsed");
    *headers[LIDX_PACKETS] = string_duplicate ("packets");
    *headers[LIDX_BYTES] = string_duplicate ("bytes");
    *headers[LIDX_REASON] = string_duplicate ("reason");
    *headers[LIDX_SERVICE_NAME] = string_duplicate ("service_name");
    *headers[LIDX_AGENT] = string_duplicate ("agent");
    *headers[LIDX_FROM] = string_duplicate ("from");
    *headers[LIDX_TO] = string_duplicate ("to");
    *headers[LIDX_SYS_MSGS] = string_duplicate ("sys_msgs");
    *headers[LIDX_FW_MESSAGE] = string_duplicate ("fw_message");
    *headers[LIDX_INTERNAL_CA] = string_duplicate ("Internal_CA:");
    *headers[LIDX_SERIAL_NUM] = string_duplicate ("serial_num:");
    *headers[LIDX_DN] = string_duplicate ("dn:");
    *headers[LIDX_ICMP] = string_duplicate ("ICMP");
    *headers[LIDX_ICMP_TYPE] = string_duplicate ("icmp-type");
    *headers[LIDX_ICMP_TYPE2] = string_duplicate ("ICMP Type");
    *headers[LIDX_ICMP_CODE] = string_duplicate ("icmp-code");
    *headers[LIDX_ICMP_CODE2] = string_duplicate ("ICMP Code");
    *headers[LIDX_MSGID] = string_duplicate ("msgid");
    *headers[LIDX_MESSAGE_INFO] = string_duplicate ("message_info");
    *headers[LIDX_LOG_SYS_MESSAGE] = string_duplicate ("log_sys_message");
    *headers[LIDX_SESSION_ID] = string_duplicate ("session_id:");
    *headers[LIDX_DNS_QUERY] = string_duplicate ("dns_query");
    *headers[LIDX_DNS_TYPE] = string_duplicate ("dns_type");
    *headers[LIDX_SCHEME] = string_duplicate ("scheme:");
    *headers[LIDX_SRCKEYID] = string_duplicate ("srckeyid");
    *headers[LIDX_DSTKEYID] = string_duplicate ("dstkeyid");
    *headers[LIDX_METHODS] = string_duplicate ("methods:");
    *headers[LIDX_PEER_GATEWAY] = string_duplicate ("peer gateway");
    *headers[LIDX_IKE] = string_duplicate ("IKE:");
    *headers[LIDX_IKE_IDS] = string_duplicate ("IKE IDs:");
    *headers[LIDX_ENCRYPTION_FAILURE] =
        string_duplicate ("encryption failure:");
    *headers[LIDX_ENCRYPTION_FAIL_R] =
        string_duplicate ("encryption fail reason:");
    *headers[LIDX_COOKIEI] = string_duplicate ("CookieI");
    *headers[LIDX_COOKIER] = string_duplicate ("CookieR");
    *headers[LIDX_START_TIME] = string_duplicate ("start_time");
    *headers[LIDX_SEGMENT_TIME] = string_duplicate ("segment_time");
    *headers[LIDX_CLIENT_IN_PACKETS] =
        string_duplicate ("client_inbound_packets");
    *headers[LIDX_CLIENT_OUT_PACKETS] =
        string_duplicate ("client_outbound_packets");
    *headers[LIDX_CLIENT_IN_BYTES] = string_duplicate ("client_inbound_bytes");
    *headers[LIDX_CLIENT_OUT_BYTES] =
        string_duplicate ("client_outbound_bytes");
    *headers[LIDX_CLIENT_IN_IF] = string_duplicate ("client_inbound_interface");
    *headers[LIDX_CLIENT_OUT_IF] =
        string_duplicate ("client_outbound_interface");
    *headers[LIDX_SERVER_IN_PACKETS] =
        string_duplicate ("server_inbound_packets");
    *headers[LIDX_SERVER_OUT_PACKETS] =
        string_duplicate ("server_outbound_packets");
    *headers[LIDX_SERVER_IN_BYTES] = string_duplicate ("server_inbound_bytes");
    *headers[LIDX_SERVER_OUT_BYTES] =
        string_duplicate ("server_outbound_bytes");
    *headers[LIDX_SERVER_IN_IF] = string_duplicate ("server_inbound_interface");
    *headers[LIDX_SERVER_OUT_IF] =
        string_duplicate ("server_outbound_interface");
    *headers[LIDX_MESSAGE] = string_duplicate ("message");
    *headers[LIDX_USER] = string_duplicate ("user");
    *headers[LIDX_SRCNAME] = string_duplicate ("srcname");
    *headers[LIDX_OM] = string_duplicate ("OM:");
    *headers[LIDX_OM_METHOD] = string_duplicate ("om_method:");
    *headers[LIDX_ASSIGNED_IP] = string_duplicate ("assigned_IP:");
    *headers[LIDX_VPN_USER] = string_duplicate ("vpn_user");
    *headers[LIDX_MAC] = string_duplicate ("MAC:");
    *headers[LIDX_ATTACK] = string_duplicate ("attack");
    *headers[LIDX_ATTACK_INFO] = string_duplicate ("Attack Info");
    *headers[LIDX_CLUSTER_INFO] = string_duplicate ("Cluster_Info");
    *headers[LIDX_DCE_RPC_UUID] = string_duplicate ("DCE-RPC Interface UUID");
    *headers[LIDX_DCE_RPC_UUID_1] =
        string_duplicate ("DCE-RPC Interface UUID-1");
    *headers[LIDX_DCE_RPC_UUID_2] =
        string_duplicate ("DCE-RPC Interface UUID-2");
    *headers[LIDX_DCE_RPC_UUID_3] =
        string_duplicate ("DCE-RPC Interface UUID-3");
    *headers[LIDX_DURING_SEC] = string_duplicate ("during_sec");
    *headers[LIDX_FRAGMENTS_DROPPED] = string_duplicate ("fragments_dropped");
    *headers[LIDX_IP_ID] = string_duplicate ("ip_id");
    *headers[LIDX_IP_LEN] = string_duplicate ("ip_len");
    *headers[LIDX_IP_OFFSET] = string_duplicate ("ip_offset");
    *headers[LIDX_TCP_FLAGS2] = string_duplicate ("TCP flags");
    *headers[LIDX_SYNC_INFO] = string_duplicate ("sync_info:");
    *headers[LIDX_LOG] = string_duplicate ("log");
    *headers[LIDX_CPMAD] = string_duplicate ("cpmad");
    *headers[LIDX_AUTH_METHOD] = string_duplicate ("auth_method");
    *headers[LIDX_TCP_PACKET_OOS] =
        string_duplicate ("TCP packet out of state");
    *headers[LIDX_RPC_PROG] = string_duplicate ("rpc_prog");
    *headers[LIDX_TH_FLAGS] = string_duplicate ("th_flags");
    *headers[LIDX_CP_MESSAGE] = string_duplicate ("cp_message:");
    *headers[LIDX_REJECT_CATEGORY] = string_duplicate ("reject_category");
    *headers[LIDX_IKE_LOG] = string_duplicate ("IKE Log:");
    *headers[LIDX_NEGOTIATION_ID] = string_duplicate ("Negotiation Id:");
    *headers[LIDX_DECRYPTION_FAILURE] =
        string_duplicate ("decryption failure:");
    *headers[LIDX_LEN] = string_duplicate ("len");
}

/*
 * BEGIN: function to free pointers in logfile arrays
 */
void
free_lfield_arrays (char **headers[NUMBER_LIDX_FIELDS])
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function free_lfield_arrays\n");
    }

    for (i = 0; i < NUMBER_LIDX_FIELDS; i++)
    {
        if (headers[i] != NULL)
        {
            if (*headers[i] != NULL)
            {
                free (*headers[i]);
                *headers[i] = NULL;
            }
        }
        free (headers[i]);
    }
}

/*
 * BEGIN: function to initialize fields headers of audit fields
 */
void
initialize_afield_headers (char **headers[NUMBER_AIDX_FIELDS])
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_afield_headers\n");
    }

    for (i = 0; i < NUMBER_AIDX_FIELDS; i++)
    {
        headers[i] = (char**) malloc (sizeof (char *));
        *headers[i] = NULL;
    }

    *headers[AIDX_NUM] = string_duplicate ("loc");
    *headers[AIDX_TIME] = string_duplicate ("time");
    *headers[AIDX_ACTION] = string_duplicate ("action");
    *headers[AIDX_ORIG] = string_duplicate ("orig");
    *headers[AIDX_IF_DIR] = string_duplicate ("i/f_dir");
    *headers[AIDX_IF_NAME] = string_duplicate ("i/f_name");
    *headers[AIDX_HAS_ACCOUNTING] = string_duplicate ("has_accounting");
    *headers[AIDX_UUID] = string_duplicate ("uuid");
    *headers[AIDX_PRODUCT] = string_duplicate ("product");
    *headers[AIDX_OBJECTNAME] = string_duplicate ("ObjectName");
    *headers[AIDX_OBJECTTYPE] = string_duplicate ("ObjectType");
    *headers[AIDX_OBJECTTABLE] = string_duplicate ("ObjectTable");
    *headers[AIDX_OPERATION] = string_duplicate ("Operation");
    *headers[AIDX_UID] = string_duplicate ("Uid");
    *headers[AIDX_ADMINISTRATOR] = string_duplicate ("Administrator");
    *headers[AIDX_MACHINE] = string_duplicate ("Machine");
    *headers[AIDX_SUBJECT] = string_duplicate ("Subject");
    *headers[AIDX_AUDIT_STATUS] = string_duplicate ("Audit Status");
    *headers[AIDX_ADDITIONAL_INFO] = string_duplicate ("Additional Info");
    *headers[AIDX_OPERATION_NUMBER] = string_duplicate ("Operation Number");
    *headers[AIDX_FIELDSCHANGES] = string_duplicate ("FieldsChanges");
}

/*
 * BEGIN: function to free pointers in audit arrays
 */
void
free_afield_arrays (char **headers[NUMBER_AIDX_FIELDS])
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function free_afield_arrays\n");
    }

    for (i = 0; i < NUMBER_AIDX_FIELDS; i++)
    {
        if (headers[i] != NULL)
        {
            if (*headers[i] != NULL)
            {
                free (*headers[i]);
                *headers[i] = NULL;
            }
        }
        free (headers[i]);
    }
}

/*
 * BEGIN: function to initialize fields values of logfile fields
 */
void
initialize_lfield_values (char **values[NUMBER_LIDX_FIELDS])
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_lfield_values\n");
    }

    for (i = 0; i < NUMBER_LIDX_FIELDS; i++)
    {
        values[i] = (char**) malloc (sizeof (char *));
        *values[i] = NULL;
    }
}

/*
 * BEGIN: function to initialize fields values of audit fields
 */
void
initialize_afield_values (char **values[NUMBER_AIDX_FIELDS])
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_afield_values\n");
    }

    for (i = 0; i < NUMBER_AIDX_FIELDS; i++)
    {
        values[i] = (char**) malloc (sizeof (char *));
        *values[i] = NULL;
    }
}

/*
 * BEGIN: function to initialize order values of logfile fields
 */
void
initialize_lfield_order (int *order)
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_lfield_order\n");
    }

    for (i = 0; i < NUMBER_LIDX_FIELDS; i++)
    {
        order[i] = -1;
    }
}

/*
 * BEGIN: function to initialize order values of logfile fields
 */
void
initialize_afield_order (int *order)
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_afield_order\n");
    }

    for (i = 0; i < NUMBER_AIDX_FIELDS; i++)
    {
        order[i] = -1;
    }
}

/*
 * BEGIN: function to initialize output values of logfile fields
 */
void
initialize_lfield_output (int *output)
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_lfield_output\n");
    }

    for (i = 0; i < NUMBER_LIDX_FIELDS; i++)
    {
        output[i] = 0;
    }
}

/*
 * BEGIN: function to initialize output values of audit fields
 */
void
initialize_afield_output (int *output)
{
    int i;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function initialize_afield_output\n");
    }

    for (i = 0; i < NUMBER_AIDX_FIELDS; i++)
    {
        output[i] = 0;
    }
}

/*
 * BEGIN: function to cleanup environment and exit fw1-loggrabber
 */
void
exit_loggrabber (int errorcode, const std::string& entity)
{
    if (entity.length() > 0)
    {
        postEntityHealthStatus(entity, cfgvalues.status_server,
                               cfgvalues.entity_health_endpoint,
                               cfgvalues.status_server_auth_token,
                               0);
    }
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function exit_loggrabber\n");
    }

    free_lfield_arrays (lfield_headers);
    free_afield_arrays (afield_headers);
    free_lfield_arrays (lfields);
    free_afield_arrays (afields);

    if (LogfileName != NULL)
    {
        free (LogfileName);
    }
//      if (cfgvalues.fw1_logfile) {
//              free(cfgvalues.fw1_logfile);
//      }

//  for (i = 0; i < filtercount; i++)
//    {
//      if (filterarray[i])
//      {
//        free (filterarray[i]);
//      }
//    }

    while (sl)
    {
        sl = stringlist_delete (&sl);
    }

//  if (cfgvalues.config_filename != NULL) {
//    free (cfgvalues.config_filename);
//  }

//  if (cfgvalues.leaconfig_filename != NULL) {
//    free (cfgvalues.leaconfig_filename);
//  }
    exit (errorcode);
}

/*
 * initilization function to define open, submit and close handler
 */
void
logging_init_env (int logging)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function logging_init_env\n");
    }

    // Specifies which logging operation shall be executed.
    switch (logging)
    {
    case SCREEN:
        open_log = &open_screen;
        submit_log = &submit_screen;
        close_log = &close_screen;
        break;
    case LOGFILE:
        open_log = &open_logfile;
        submit_log = &submit_logfile;
        close_log = &close_logfile;
        break;
    default:
        open_log = &open_screen;
        submit_log = &submit_screen;
        close_log = &close_screen;
        break;
    }
    return;
}

/*
 * screen initializations
 */
void
open_screen ()
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function open_screen\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Open connection to screen.\n");
    }
    //We don't need to do anything here
    return;
}

void
submit_screen (char *message)
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function submit_screen\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Submit message to screen.\n");
    }
    fprintf (stdout, "%s\n", message);
    fflush (NULL);
    return;
}

void
close_screen ()
{
    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function close_screen\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Close connection to screen.\n");
    }
    //We don't need to do anything here
    return;
}

/*
 * log file initializations
 */
void
open_logfile ()
{

    char *output_file_name;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function open_logfile\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Initilize log file and open log file.\n");
    }

    //create current output filename
    output_file_name =
        (char *) malloc (strlen (cfgvalues.output_file_prefix) + 5);
    if (output_file_name == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }
    strcpy (output_file_name, cfgvalues.output_file_prefix);
    strcat (output_file_name, ".log");

    if ((logstream = fopen (output_file_name, "a+")) == NULL)
    {
        fprintf (stderr, "ERROR: Fail to open the log file.\n");
        exit_loggrabber (1);
    }

    return;
}

void
submit_logfile (char *message)
{

    int i;
    long fsize;
    // char sn[100];
    char* sn;
    char *output_file_name;

    time_t time_date;
    struct tm *current_date;
    int month;      // 1 through 12
    int day;      // 1 through max_days
    int year;     // 1500 through 2200
    int hour;     // 0 through 23
    int minute;     // 0 through 59
    int second;     // 0 through 59

    const char* indexed_format_str = "%s-%4.4d-%2.2d-%2.2d_%2.2d%2.2d%2.2d_%d.log";
    const char* format_str =         "%s-%4.4d-%2.2d-%2.2d_%2.2d%2.2d%2.2d.log";

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function submit_logfile\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Submit message to log file.\n");
    }

    fprintf (logstream, "%s\n", message);

    //Check and see if it reaches the log file limitation
    fseek (logstream, 0, SEEK_CUR);
    fsize = ftell (logstream);
    /* File size check and see whether or not it reaches maximum */
    if (fsize > cfgvalues.output_file_rotatesize)
    {
        //The log file will be refreshed.
        fclose (logstream);
        time_date = time (NULL);
        current_date = localtime (&time_date);
        month = current_date->tm_mon + 1;
        day = current_date->tm_mday;
        year = current_date->tm_year + 1900;
        hour = current_date->tm_hour;
        minute = current_date->tm_min;
        second = current_date->tm_sec;
        //create current output filename
        output_file_name =
            (char *) malloc (strlen (cfgvalues.output_file_prefix) + 5);
        strcpy (output_file_name, cfgvalues.output_file_prefix);
        strcat (output_file_name, ".log");
        //Copy log file
        int size = 1 + snprintf(NULL, 0, format_str,
                                cfgvalues.output_file_prefix, year, month, day, hour, minute,
                                second);

        sn = (char*) malloc(size);
        snprintf(sn, size, format_str,
                 cfgvalues.output_file_prefix, year, month, day, hour, minute,
                 second);

        if (fileExist (sn))
        {
            //Unfortunately, events come in too fast
            i = 1;
            int size = 1 + snprintf(NULL, 0, indexed_format_str,
                                    cfgvalues.output_file_prefix, year, month, day, hour, minute,
                                    second, i);

            sn = (char*) malloc(size);
            snprintf(sn, size, indexed_format_str,
                     cfgvalues.output_file_prefix, year, month, day, hour, minute,
                     second, i);

            while (fileExist (sn))
            {
                i++;
                snprintf (sn, size, indexed_format_str,
                          cfgvalues.output_file_prefix, year, month, day, hour,
                          minute, second, i);
            }     //end of while
        }     //end of inner if
        fileCopy (output_file_name, sn);
        //Clean log file
        if ((logstream = fopen (output_file_name, "w")) == NULL)
        {
            ;
            fprintf (stderr, "ERROR: Fail to open the log file.\n");
            exit_loggrabber (1);
        }     //end of inner if
    }       //end of if


    free(sn);
    return;
}

void
close_logfile ()
{

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function close_logfile\n");
    }

    if (cfgvalues.debug_mode)
    {
        fprintf (stderr, "DEBUG: Close the log file.\n");
    }
    fclose (logstream);

    return;
}

void
fileCopy (const char *inputFile, const char *outputFile)
{

    // The most recent character from input file to output file
    char x;

    // fpi points to the input file, fpo points to the output file
    FILE *fpi, *fpo;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function fileCopy\n");
    }

    // Open input file, read-only
    fpi = fopen (inputFile, "r");
    // Open output file, write-only
    fpo = fopen (outputFile, "w");

    // Get next char from input file, store in x, until we reach EOF
    while ((x = getc (fpi)) != -1)
    {
        putc (x, fpo);
    }

    //After we have done, close both files
    fclose (fpi);
    fclose (fpo);

    return;
}

int
fileExist (const char *fileName)
{
    FILE *infile;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function fileExist\n");
    }

    infile = fopen (fileName, "r");
    if (infile == NULL)
    {
        return FALSE;
    }
    else
    {
        fclose (infile);
        return TRUE;
    }       //end of if
}

char
getschar ()
{
    char c;
    char ch;

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function getschar\n");
    }

    ch = getchar ();
    while ((c = getchar ()) != '\n' && c != EOF);
    return ch;
}

void
check_lea_config (char *leaconf)
{
    char *configdir;
    char *tempdir;
    char *opsecdir;
    char *opsecfile = NULL;
    char *tmpleaconf = NULL;
    char *tmploggrabberconf = NULL;
    char filebuff[1024];
    char *tempbuffer;

    FILE *filetest;

#ifdef WIN32
    TCHAR buff[BUFSIZE];
    DWORD dwRet;
#else
    long size;
#endif

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function check_config_files\n");
    }

    configdir = getenv ("LOGGRABBER_CONFIG_PATH");
    tempdir = getenv ("LOGGRABBER_TEMP_PATH");
    opsecdir = getenv ("OPSECDIR");

#ifdef WIN32
    dwRet = GetCurrentDirectory (BUFSIZE, buff);

    if (dwRet == 0)
    {
        fprintf (stderr,
                 "ERROR: Cannot get current working directory (error code: %d)\n",
                 GetLastError ());
        exit_loggrabber (1);
    }
    if (dwRet > BUFSIZE)
    {
        fprintf (stderr,
                 "ERROR: Getting the current working directory failed failed since buffer too small, and it needs %d chars\n",
                 dwRet);
        exit_loggrabber (1);
    }

    if ((tmpleaconf = (char *) malloc (BUFSIZE)) == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }

    // no lea.conf specified via function parameter, default to 'lea.conf' in cwd
    if (leaconf == NULL)
    {
        strcpy (tmpleaconf, buff);
        strcat (tmpleaconf, "\\lea.conf");
    }
    // lea.conf specified via function parameter
    else
    {
        // first characters of lea.conf filename are '\' or '.:\' -> absolute path
        if ((leaconf[0] == '\\') ||
                ((leaconf[1] == ':') && (leaconf[2] == '\\')))
        {
            strcpy (tmpleaconf, leaconf);
        }
        // otherwise append the relative path to cwd
        else
        {
            strcpy (tmpleaconf, buff);
            strcat (tmpleaconf, "\\");
            strcat (tmpleaconf, leaconf);
        }
    }
#else
    size = pathconf (".", _PC_PATH_MAX);
    if ((tmpleaconf = (char *) malloc ((size_t) size)) == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }

    // no lea.conf specified via function parameter, default to 'lea.conf' in cwd
    if (leaconf == NULL)
    {
        if (getcwd (tmpleaconf, (size_t) size) == NULL)
        {
            fprintf (stderr, "ERROR: Cannot get current working directory\n");
            exit_loggrabber (1);
        }
        strcat (tmpleaconf, "/lea.conf");
    }
    // lea.conf specified via function parameter
    else
    {
        // first character of lea.conf filename is '/' -> absolute path
        if (leaconf[0] == '/')
        {
            strcpy (tmpleaconf, leaconf);
        }
        // otherwise append the relative path to cwd
        else
        {
            if (getcwd (tmpleaconf, (size_t) size) == NULL)
            {
                fprintf (stderr,
                         "ERROR: Cannot get current working directory\n");
                exit_loggrabber (1);
            }
            strcat (tmpleaconf, "/");
            strcat (tmpleaconf, leaconf);
        }
    }
#endif

    // cannot read lea.conf, so try to look into LOGGRABBER_CONFIG_PATH
    if ((filetest = fopen (tmpleaconf, "r")) == NULL)
    {
        if (configdir != NULL)
        {
            strcpy (tmpleaconf, configdir);
#ifdef WIN32
            strcat (tmpleaconf, "\\");
#else
            strcat (tmpleaconf, "/");
#endif
            strcat (tmpleaconf, leaconf);

            // also cannot read lea.conf in LOGGRABBER_CONFIG_PATH
            if ((filetest = fopen (tmpleaconf, "r")) == NULL)
            {
                fprintf (stderr,
                         "ERROR: Cannot open LEA configuration file (%s)\n",
                         leaconf);
                fprintf (stderr,
                         "       Specify either a absolute lea.conf path on commandline,\n");
                fprintf (stderr,
                         "       or place lea.conf into current working directory\n");
                fprintf (stderr,
                         "       or $LOGGRABBER_CONFIG_PATH directory.\n");
                exit_loggrabber (1);
            }
            else
            {
                fclose (filetest);
            }
        }
        else
        {
            fprintf (stderr, "ERROR: Cannot open LEA configuration file (%s)\n",
                     leaconf);
            fprintf (stderr,
                     "       Specify either a absolute lea.conf path on commandline,\n");
            fprintf (stderr,
                     "       or place lea.conf into current working directory\n");
            fprintf (stderr, "       or $LOGGRABBER_CONFIG_PATH directory.\n");
            exit_loggrabber (1);
        }
    }
    else
    {
        fclose (filetest);
    }

    // open lea.conf in order to get opsec_sslca_file value
    if ((filetest = fopen (tmpleaconf, "r")) == NULL)
    {
        fprintf (stderr, "ERROR: Cannot open LEA configuration file (%s)\n",
                 leaconf);
        fprintf (stderr,
                 "       Specify either a absolute lea.conf path on commandline,\n");
        fprintf (stderr,
                 "       or place lea.conf into current working directory\n");
        fprintf (stderr, "       or $LOGGRABBER_CONFIG_PATH directory.\n");
        exit_loggrabber (1);
    }
    else
    {
        while (fgets (filebuff, 1023, filetest))
        {
            if (string_incmp (filebuff, "opsec_sslca_file", 16) == 0)
            {
                opsecfile = string_trim (filebuff + (16 * sizeof (char)), ' ');
                break;
            }
        }
    }

    // no opsec_sslca_file specified in lea.conf, so we probably don't need one...
    if (opsecfile != NULL)
    {
#ifdef WIN32
        // first characters of opsec_sslca_file filename are '\' or '.:\' -> absolute path
        if (!((opsecfile[0] == '\\') ||
                ((opsecfile[1] == ':') && (opsecfile[2] == '\\'))))
        {
            fprintf (stderr,
                     "WARNING: You specified a relative path for opsec_sslca_file in\n");
            fprintf (stderr, "         %s. When not using an\n", tmpleaconf);
            fprintf (stderr,
                     "         absolute path, the certificate will be searched in\n");
            fprintf (stderr,
                     "         $LOGGRABBER_TEMP_PATH or in current working.\n");
            fprintf (stderr,
                     "         directory if $LOGGRABBER_TEMP_PATH is not set.\n");
        }
#else
        // first character of opsec_sslca_file filename is '/' -> absolute path
        if (!(opsecfile[0] == '/'))
        {
            fprintf (stderr,
                     "WARNING: You specified a relative path for opsec_sslca_file in\n");
            fprintf (stderr, "         %s. When not using an\n", tmpleaconf);
            fprintf (stderr,
                     "         absolute path, the certificate will be searched in\n");
            fprintf (stderr,
                     "         $LOGGRABBER_TEMP_PATH or in current working.\n");
            fprintf (stderr,
                     "         directory if $LOGGRABBER_TEMP_PATH is not set.\n");
        }
#endif
    }

    cfgvalues.leaconfig_filename = string_duplicate (tmpleaconf);

    if (debug_mode > 0)
    {
        fprintf (stderr, "DEBUG: LEA configuration file is: %s\n",
                 cfgvalues.leaconfig_filename);
    }

    unsigned int tempbufferSize = 0;
    if (tempdir != NULL)
    {
        /*
        NOTE: this is NOT a memory leak.
        this is used in putEnv, which requires a long-lived
        variable.
        */
        tempbufferSize = strlen (tempdir) + 10;
        tempbuffer = (char*) malloc (tempbufferSize);
        if (tempbuffer == NULL)
        {
            fprintf (stderr, "ERROR: Out of memory\n");
            exit_loggrabber (1);
        }
        else
        {
            snprintf (tempbuffer, tempbufferSize, "OPSECDIR=%s", tempdir);
            putenv (tempbuffer);
        }
    }

    if (tmpleaconf != NULL)
    {
        free (tmpleaconf);
    }

}

void
check_loggrabber_config (char *loggrabberconf)
{
    char *configdir;
    char *tempdir;
    char *opsecdir;
    char *opsecfile = NULL;
    char *tmploggrabberconf = NULL;
    char filebuff[1024];
    char *tempbuffer;

    FILE *filetest;

#ifdef WIN32
    TCHAR buff[BUFSIZE];
    DWORD dwRet;
#else
    long size;
#endif

    if (cfgvalues.debug_mode >= 2)
    {
        fprintf (stderr, "DEBUG: function check_config_files\n");
    }

    configdir = getenv ("LOGGRABBER_CONFIG_PATH");
    tempdir = getenv ("LOGGRABBER_TEMP_PATH");
    opsecdir = getenv ("OPSECDIR");

#ifdef WIN32
    dwRet = GetCurrentDirectory (BUFSIZE, buff);

    if (dwRet == 0)
    {
        fprintf (stderr,
                 "ERROR: Cannot get current working directory (error code: %d)\n",
                 GetLastError ());
        exit_loggrabber (1);
    }
    if (dwRet > BUFSIZE)
    {
        fprintf (stderr,
                 "ERROR: Getting the current working directory failed failed since buffer too small, and it needs %d chars\n",
                 dwRet);
        exit_loggrabber (1);
    }

    if ((tmploggrabberconf = (char *) malloc (BUFSIZE)) == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }

    // no fw1-loggrabber.conf specified via function parameter, default to 'fw1-loggrabber.conf' in cwd
    if (loggrabberconf == NULL)
    {
        strcpy (tmploggrabberconf, buff);
        strcat (tmploggrabberconf, "\\fw1-loggrabber.conf");
    }
    // fw1-loggrabber.conf specified via function parameter
    else
    {
        // first characters of fw1-loggrabber.conf filename are '\' or '.:\' -> absolute path
        if ((loggrabberconf[0] == '\\') ||
                ((loggrabberconf[1] == ':') && (loggrabberconf[2] == '\\')))
        {
            strcpy (tmploggrabberconf, loggrabberconf);
        }
        // otherwise append the relative path to cwd
        else
        {
            strcpy (tmploggrabberconf, buff);
            strcat (tmploggrabberconf, "\\");
            strcat (tmploggrabberconf, loggrabberconf);
        }
    }
#else
    size = pathconf (".", _PC_PATH_MAX);
    if ((tmploggrabberconf = (char *) malloc ((size_t) size)) == NULL)
    {
        fprintf (stderr, "ERROR: Out of memory\n");
        exit_loggrabber (1);
    }

    // no fw1-loggrabber.conf specified via function parameter, default to 'fw1-loggrabber.conf' in cwd
    if (loggrabberconf == NULL)
    {
        if (getcwd (tmploggrabberconf, (size_t) size) == NULL)
        {
            fprintf (stderr, "ERROR: Cannot get current working directory\n");
            exit_loggrabber (1);
        }
        strcat (tmploggrabberconf, "/fw1-loggrabber.conf");
    }
    // fw1-loggrabber.conf specified via function parameter
    else
    {
        // first character of fw1-loggrabber.conf filename is '/' -> absolute path
        if (loggrabberconf[0] == '/')
        {
            strcpy (tmploggrabberconf, loggrabberconf);
        }
        // otherwise append the relative path to cwd
        else
        {
            if (getcwd (tmploggrabberconf, (size_t) size) == NULL)
            {
                fprintf (stderr,
                         "ERROR: Cannot get current working directory\n");
                exit_loggrabber (1);
            }
            strcat (tmploggrabberconf, "/");
            strcat (tmploggrabberconf, loggrabberconf);
        }
    }
#endif

    // cannot read loggrabber.conf, so try to look into LOGGRABBER_CONFIG_PATH
    if ((filetest = fopen (tmploggrabberconf, "r")) == NULL)
    {
        if (configdir != NULL)
        {
            strcpy (tmploggrabberconf, configdir);
#ifdef WIN32
            strcat (tmploggrabberconf, "\\");
#else
            strcat (tmploggrabberconf, "/");
#endif
            strcat (tmploggrabberconf, loggrabberconf);

            // also cannot read fw1-loggrabber.conf in LOGGRABBER_CONFIG_PATH
            if ((filetest = fopen (tmploggrabberconf, "r")) == NULL)
            {
                fprintf (stderr,
                         "ERROR: Cannot open FW1-Loggrabber configuration file (%s)\n",
                         loggrabberconf);
                fprintf (stderr,
                         "       Specify either a absolute fw1-loggrabber.conf path on commandline,\n");
                fprintf (stderr,
                         "       or place fw1-loggrabber.conf into current working directory\n");
                fprintf (stderr,
                         "       or $LOGGRABBER_CONFIG_PATH directory.\n");
                exit_loggrabber (1);
            }
            else
            {
                fclose (filetest);
            }
        }
        else
        {
            fprintf (stderr,
                     "ERROR: Cannot open FW1-Loggrabber configuration file (%s)\n",
                     loggrabberconf);
            fprintf (stderr,
                     "       Specify either a absolute fw1-loggrabber.conf path on commandline,\n");
            fprintf (stderr,
                     "       or place fw1-loggrabber.conf into current working directory\n");
            fprintf (stderr, "       or $LOGGRABBER_CONFIG_PATH directory.\n");
            exit_loggrabber (1);
        }
    }
    else
    {
        fclose (filetest);
    }

    cfgvalues.config_filename = string_duplicate (tmploggrabberconf);

    if (debug_mode > 0)
    {
        fprintf (stderr, "DEBUG: LOGGRABBER configuration file is: %s\n",
                 cfgvalues.config_filename);
    }

    const char* dir_format_str = "OPSECDIR=%s";
    unsigned int tempbufferSize = 0;
    if (tempdir != NULL)
    {
        tempbufferSize = strlen (tempdir) + strlen(dir_format_str);
        tempbuffer = (char*) malloc (tempbufferSize);
        if (tempbuffer == NULL)
        {
            fprintf (stderr, "ERROR: Out of memory\n");
            exit_loggrabber (1);
        }
        else
        {
            snprintf (tempbuffer, tempbufferSize, dir_format_str, tempdir);
            putenv (tempbuffer);
        }
    }

    if (tmploggrabberconf != NULL)
    {
        free (tmploggrabberconf);
    }
}
