/*                      _             _
**  _ __ ___   ___   __| |    ___ ___| |  mod_ssl
** | '_ ` _ \ / _ \ / _` |   / __/ __| |  Apache Interface to OpenSSL
** | | | | | | (_) | (_| |   \__ \__ \ |  www.modssl.org
** |_| |_| |_|\___/ \__,_|___|___/___/_|  ftp.modssl.org
**                      |_____|
**  ssl_engine_rand.c
**  Random Number Generator Seeding
*/

/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2001 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 */
                             /* ``The generation of random
                                  numbers is too important
                                  to be left to chance.'' */

#include "mod_ssl.h"

/*  _________________________________________________________________
**
**  Support for better seeding of SSL library's RNG
**  _________________________________________________________________
*/

static int ssl_rand_choosenum(int, int);
static int ssl_rand_feedfp(apr_pool_t *, apr_file_t *, int);

int ssl_rand_seed(server_rec *s, apr_pool_t *p, ssl_rsctx_t nCtx, char *prefix)
{
    SSLModConfigRec *mc;
    apr_array_header_t *apRandSeed;
    ssl_randseed_t *pRandSeeds;
    ssl_randseed_t *pRandSeed;
    unsigned char stackdata[256];
    int nReq, nDone;
    apr_file_t *fp;
    int i, n, l;
    time_t t;
    pid_t pid;

    mc = myModConfig(s);
    nReq  = 0;
    nDone = 0;
    apRandSeed = mc->aRandSeed;
    pRandSeeds = (ssl_randseed_t *)apRandSeed->elts;
    for (i = 0; i < apRandSeed->nelts; i++) {
        pRandSeed = &pRandSeeds[i];
        if (pRandSeed->nCtx == nCtx) {
            nReq += pRandSeed->nBytes;
            if (pRandSeed->nSrc == SSL_RSSRC_FILE) {
                /*
                 * seed in contents of an external file
                 */
                if (apr_file_open(&fp, pRandSeed->cpPath, 
                                  APR_READ, APR_OS_DEFAULT, p) != APR_SUCCESS)
                    continue;
                nDone += ssl_rand_feedfp(p, fp, pRandSeed->nBytes);
                apr_file_close(fp);
            }
            else if (pRandSeed->nSrc == SSL_RSSRC_EXEC) {
                /*
                 * seed in contents generated by an external program
                 */
                if ((fp = ssl_util_ppopen(s, p, apr_psprintf(p, "%s %d",
                                          pRandSeed->cpPath, pRandSeed->nBytes))) == NULL)
                    continue;
                nDone += ssl_rand_feedfp(p, fp, pRandSeed->nBytes);
                ssl_util_ppclose(s, p, fp);
            }
#if SSL_LIBRARY_VERSION >= 0x00905100
            else if (pRandSeed->nSrc == SSL_RSSRC_EGD) {
                /*
                 * seed in contents provided by the external
                 * Entropy Gathering Daemon (EGD)
                 */
                if ((n = RAND_egd(pRandSeed->cpPath)) == -1)
                    continue;
                nDone += n;
            }
#endif
            else if (pRandSeed->nSrc == SSL_RSSRC_BUILTIN) {
                /*
                 * seed in the current time (usually just 4 bytes)
                 */
                t = time(NULL);
                l = sizeof(time_t);
                RAND_seed((unsigned char *)&t, l);
                nDone += l;

                /*
                 * seed in the current process id (usually just 4 bytes)
                 */
                pid = mc->pid;
                l = sizeof(pid_t);
                RAND_seed((unsigned char *)&pid, l);
                nDone += l;
                
                /*
                 * seed in some current state of the run-time stack (128 bytes)
                 */
                n = ssl_rand_choosenum(0, sizeof(stackdata)-128-1);
                RAND_seed(stackdata+n, 128);
                nDone += 128;

                /*
                 * seed in an 1KB extract of the current scoreboard
                 */
                if (ap_scoreboard_image != NULL) {
                    n = ssl_rand_choosenum(0, SCOREBOARD_SIZE-1024-1);
                    RAND_seed((unsigned char *)ap_scoreboard_image+n, 1024);
                    nDone += 1024;
                }
            }
        }
    }
    ssl_log(s, SSL_LOG_INFO, "%sSeeding PRNG with %d bytes of entropy", prefix, nDone);

#if SSL_LIBRARY_VERSION >= 0x00905100
    if (RAND_status() == 0)
        ssl_log(s, SSL_LOG_WARN, "%sPRNG still contains not sufficient entropy!", prefix);
#endif
    return nDone;
}

#define BUFSIZE 8192

static int ssl_rand_feedfp(apr_pool_t *p, apr_file_t *fp, int nReq)
{
    apr_size_t nDone;
    unsigned char caBuf[BUFSIZE];
    apr_size_t nBuf;
    apr_size_t nRead;
    apr_size_t nTodo;

    nDone = 0;
    nRead = BUFSIZE;
    nTodo = nReq;
    while (1) {
        if (nReq > 0)
            nRead = (nTodo < BUFSIZE ? nTodo : BUFSIZE);
        nBuf = nRead;
        if (apr_file_read(fp, caBuf, &nBuf) != APR_SUCCESS)
            break;
        RAND_seed(caBuf, nBuf);
        nDone += nBuf;
        if (nReq > 0) {
            nTodo -= nBuf;
            if (nTodo <= 0)
                break;
        }
    }
    return nDone;
}

static int ssl_rand_choosenum(int l, int h)
{
    int i;
    char buf[50];

    srand((unsigned int)time(NULL));
    apr_snprintf(buf, sizeof(buf), "%.0f",
                 (((double)(rand()%RAND_MAX)/RAND_MAX)*(h-l)));
    i = atoi(buf)+1;
    if (i < l) i = l;
    if (i > h) i = h;
    return i;
}

