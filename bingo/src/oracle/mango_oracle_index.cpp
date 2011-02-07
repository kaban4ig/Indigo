/****************************************************************************
 * Copyright (C) 2009-2011 GGA Software Services LLC
 * 
 * This file is part of Indigo toolkit.
 * 
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 3 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 * 
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 ***************************************************************************/

#include "oracle/bingo_oracle.h"

#include "base_cpp/scanner.h"
#include "base_cpp/profiling.h"
#include "base_cpp/os_sync_wrapper.h"
#include "molecule/molecule_auto_loader.h"
#include "molecule/molfile_loader.h"
#include "molecule/molecule_stereocenters.h"
#include "molecule/cmf_saver.h"
#include "molecule/smiles_loader.h"
#include "molecule/icm_loader.h"
#include "core/mango_index.h"
#include "oracle/ora_wrap.h"
#include "oracle/ora_logger.h"
#include "oracle/mango_oracle.h"
#include "oracle/mango_shadow_table.h"
#include "oracle/bingo_oracle_context.h"
#include "oracle/bingo_fingerprints.h"
#include "oracle/rowid_saver.h"
#include "oracle/mango_oracle_index_parallel.h"
#include "oracle/bingo_profiling.h"
#include "molecule/elements.h"
#include "base_cpp/auto_ptr.h"
#include "oracle/mango_fetch_context.h"

bool mangoPrepareMolecule (OracleEnv &env, const char *rowid,
                            const Array<char> &molfile_buf,
                            MangoOracleContext &context,
                            MangoIndex &index,
                            Array<char> &data,
                            OsLock *lock_for_exclusive_access)
{
   profTimerStart(tall, "moleculeIndex.prepare");

   ArrayOutput output(data);

   output.writeChar(0); // 0 -- present, 1 -- removed from index

   QS_DEF(Array<char>, compressed_rowid);
   ArrayOutput rid_output(compressed_rowid);

   {
      // RowIDSaver modifies context.context().rid_dict and 
      // requires exclusive access for this
      OsLockerNullable locker(lock_for_exclusive_access);

      RowIDSaver rid_saver(context.context().rid_dict, rid_output);

      rid_saver.saveRowID(rowid);
   }

   output.writeByte((byte)compressed_rowid.size());
   output.writeArray(compressed_rowid);

   TRY_READ_TARGET_MOL
   {
      BufferScanner scanner(molfile_buf);

      try
      {
         index.prepare(scanner, output, lock_for_exclusive_access);
      }
      catch (CmfSaver::Error &e) 
      {
         OsLockerNullable locker(lock_for_exclusive_access);
         env.dbgPrintf(bad_molecule_warning_rowid, rowid, e.message());
         return false;
      }
   }
   CATCH_READ_TARGET_MOL_ROWID(rowid, return false);

   // some magic: round it up to avoid ora-22282
   if (data.size() % 2 == 1)
      output.writeChar(0);
   
   return true;
}

void mangoRegisterMolecule (OracleEnv &env, const char *rowid,
                             MangoOracleContext &context,
                             const MangoIndex &index,
                             BingoFingerprints &fingerprints,
                             const Array<char> &prepared_data)
{
   profTimerStart(tall, "moleculeIndex.register");

   int blockno, offset;

   profTimerStart(tstor, "moleculeIndex.register_storage");
   context.context().storage.add(env, prepared_data, blockno, offset);
   profTimerStop(tstor);

   profTimerStart(tfing, "moleculeIndex.register_fingerprint");
   fingerprints.addFingerprint(env, index.getFingerprint());
   profTimerStop(tfing);

   profTimerStart(tshad, "moleculeIndex.register_shadow");
   context.shadow_table.addMolecule(env, index, rowid, blockno + 1, offset);
   profTimerStop(tshad);
}

bool mangoPrepareAndRegisterMolecule (OracleEnv &env, const char *rowid,
                             const Array<char> &molfile_buf,
                             MangoOracleContext &context,
                             MangoIndex &index,
                             BingoFingerprints &fingerprints)
{
   QS_DEF(Array<char>, prepared_data);
   
   if (mangoPrepareMolecule(env, rowid, molfile_buf, context, 
      index, prepared_data, NULL))
   {
      mangoRegisterMolecule(env, rowid, context, 
         index, fingerprints, prepared_data);

      return true;
   }

   return false;
}


void mangoRegisterTable (OracleEnv &env, MangoOracleContext &context)
{
   profTimerStart(tall, "total");

   QS_DEF(Array<char>, source_table);
   QS_DEF(Array<char>, source_column);
   QS_DEF(Array<char>, target_datatype);
   QS_DEF(Array<char>, molfile_buf);
   OracleStatement statement(env);
   AutoPtr<OracleLOB> molfile_lob;
   OraRowidText rowid;
   char varchar2_text[4001];

   context.context().getSourceTable(env, source_table);
   context.context().getSourceColumn(env, source_column);
   context.context().getTargetDatatype(env, target_datatype);

   bool blob = (strcmp(target_datatype.ptr(), "BLOB") == 0);
   bool clob = (strcmp(target_datatype.ptr(), "CLOB") == 0);

   int total_count = 0;

   OracleStatement::executeSingleInt(total_count, env, "SELECT COUNT(*) FROM %s", source_table.ptr());

   context.context().longOpInit(env, total_count, "Building molecule index",
      source_table.ptr(), "molecules");

   if (blob || clob)
      statement.append("SELECT %s, RowidToChar(rowid) FROM %s ",
                       source_column.ptr(), source_table.ptr());
   else
      statement.append("SELECT COALESCE(%s, ' '), RowidToChar(rowid) FROM %s ",
                       source_column.ptr(), source_table.ptr());
                     //"ORDER BY dbms_rowid.rowid_block_number(rowid), dbms_rowid.rowid_row_number(rowid)",

   statement.prepare();
   
   if (blob)
   {
      molfile_lob.reset(new OracleLOB(env));
      statement.defineBlobByPos(1, molfile_lob.ref());
   }
   else if (clob)
   {
      molfile_lob.reset(new OracleLOB(env));
      statement.defineClobByPos(1, molfile_lob.ref());
   }
   else
      statement.defineStringByPos(1, varchar2_text, sizeof(varchar2_text));
   
   statement.defineStringByPos(2, rowid.ptr(), sizeof(rowid));

   BingoFingerprints &fingerprints = context.fingerprints;

   fingerprints.validateForUpdate(env);

   if (context.context().nthreads == 1)
   {
      int n = 0;

      MangoIndex index(context.context());

      if (statement.executeAllowNoData()) do
      {
         env.dbgPrintf("inserting molecule #%d with rowid %s\n", n, rowid.ptr());

         if (blob || clob)
            molfile_lob->readAll(molfile_buf, false);
         else
            molfile_buf.readString(varchar2_text, false);

         try
         {
            mangoPrepareAndRegisterMolecule(env, rowid.ptr(),
                  molfile_buf, context, index, fingerprints);
         }
         catch (Exception &ex)
         {
            char buf[4096];
            snprintf(buf, NELEM(buf), "Failed on record with rowid=%s. Error message is '%s'",
               rowid.ptr(), ex.message());

            throw Exception(buf);
         }
         n++;

         if ((n % 50) == 0)
            context.context().longOpUpdate(env, n);
         
         if ((n % 1000) == 0)
         {
            env.dbgPrintf("done %d molecules; flushing\n", n);
            context.context().storage.flush(env);
         }
      } while (statement.fetch());
   }
   else
   {
      if (statement.executeAllowNoData())
      {
         MangoRegisterDispatcher dispatcher(context, env, rowid.ptr());
         dispatcher.setup(&statement, molfile_lob.get(), 
            varchar2_text, blob || clob);
         
         int nthreads = context.context().nthreads;

         if (nthreads <= 0)
            dispatcher.run();
         else
            dispatcher.run(nthreads);
      }
   }

   fingerprints.flush(env);
   context.shadow_table.flush(env);
}

ORAEXT void oraMangoCreateIndex (OCIExtProcContext *ctx,
                                 int context_id,
                                 const char *params, short params_ind)
{
   ORABLOCK_BEGIN
   {
      profTimersReset();
      OracleEnv env(ctx, logger);

      env.dbgPrintfTS("Creating index\n");

      BingoOracleContext &bcontext = BingoOracleContext::get(env, context_id, false, 0);

      // parse parameters before creating MangoOracleContext because
      // it creates the BingoSreening according to fingerprintSize()
      if (params_ind == OCI_IND_NOTNULL)
         bcontext.parseParameters(env, params);
      
      MangoOracleContext &context = MangoOracleContext::get(env, context_id, false);

      // save atomic mass parameters for the indexed table
      bcontext.atomicMassSave(env);

      BingoStorage &storage = context.context().storage;
      BingoFingerprints &fingerprints = context.fingerprints;
      
      context.shadow_table.drop(env);
      context.shadow_table.create(env);

      fingerprints.drop(env);
      fingerprints.create(env);

      storage.drop(env);
      storage.create(env);
      storage.validateForInsert(env);
      
      mangoRegisterTable(env, context);

      storage.finish(env);
      context.context().saveCmfDict(env);
      context.context().saveRidDict(env);
      env.dbgPrintfTS("Creating internal indices... ");
      context.shadow_table.createIndices(env);
      env.dbgPrintfTS("done\n");
      OracleStatement::executeSingle(env, "COMMIT");
      MangoFetchContext::removeByContextID(context_id);
      MangoContext::remove(context_id);
      BingoContext::remove(context_id);

      bingoProfilingPrintStatistics(false);
   }
   ORABLOCK_END
}

ORAEXT void oraMangoDropIndex (OCIExtProcContext *ctx, int context_id)
{
   ORABLOCK_BEGIN
   {
      OracleEnv env(ctx, logger);

      env.dbgPrintfTS("Dropping index #%d\n", context_id);

      MangoOracleContext &context = MangoOracleContext::get(env, context_id, false);

      context.shadow_table.drop(env);
      context.context().storage.drop(env);
      context.fingerprints.drop(env);

      MangoFetchContext::removeByContextID(context_id);
      MangoContext::remove(context_id);
      BingoContext::remove(context_id);

      // TEMP: remove CMF dictionary
      OracleStatement::executeSingle(env, "DELETE FROM CONFIG_BLOB WHERE n=%d", context_id);

   }
   ORABLOCK_END
}

ORAEXT void oraMangoTruncateIndex (OCIExtProcContext *ctx, int context_id)
{
   ORABLOCK_BEGIN
   {
      OracleEnv env(ctx, logger);

      env.dbgPrintfTS("Truncating index #%d\n", context_id);

      MangoOracleContext &context = MangoOracleContext::get(env, context_id, false);

      context.shadow_table.truncate(env);
      context.context().storage.truncate(env);
      context.fingerprints.truncate(env);

      MangoContext::remove(context_id);
      BingoContext::remove(context_id);
   }
   ORABLOCK_END
}


ORAEXT void oraMangoIndexInsert (OCIExtProcContext *ctx, int context_id,
                                 const char    *rowid,      short rowid_ind,
                                 OCILobLocator *target_loc, short target_ind)
{
   ORABLOCK_BEGIN
   {
      profTimersReset();

      OracleEnv env(ctx, logger);

      if (rowid_ind != OCI_IND_NOTNULL)
         throw BingoError("null rowid given");

      MangoOracleContext &context = MangoOracleContext::get(env, context_id, true);

      env.dbgPrintf("inserting molecule with rowid %s\n", rowid);
      
      MangoIndex index(context.context());
      BingoFingerprints &fingerprints = context.fingerprints;
      BingoStorage &storage = context.context().storage;

      storage.lock(env);
      storage.validateForInsert(env);

      fingerprints.validateForUpdate(env);

      QS_DEF(Array<char>, target_buf);

      OracleLOB target_lob(env, target_loc);

      target_lob.readAll(target_buf, false);

      mangoPrepareAndRegisterMolecule(env, rowid, target_buf, 
         context, index, fingerprints);

      storage.finish(env);
      context.shadow_table.flush(env);
      
      //fingerprints.flush(env);

      if (context.context().cmf_dict.isModified())
         context.context().saveCmfDict(env);
      if (context.context().rid_dict.isModified())
         context.context().saveRidDict(env);

      bingoProfilingPrintStatistics(false);
   }
   ORABLOCK_END
}

ORAEXT void oraMangoIndexDelete (OCIExtProcContext *ctx, int context_id,
                                 const char *rowid, short rowid_ind)
{
   ORABLOCK_BEGIN
   {
      OracleEnv env(ctx, logger);

      if (rowid_ind != OCI_IND_NOTNULL)
         throw BingoError("null rowid given");

      MangoOracleContext &context = MangoOracleContext::get(env, context_id, false);

      int blockno, offset;

      if (context.shadow_table.getMoleculeLocation(env, rowid, blockno, offset))
      {
         env.dbgPrintf("deleting molecule that has rowid %s\n", rowid);
         
         BingoStorage &storage = context.context().storage;

         storage.lock(env);
         storage.markRemoved(env, blockno, offset);

         context.shadow_table.deleteMolecule(env, rowid);
      }
      else
         env.dbgPrintf("molecule with rowid %s not found\n", rowid);
   }
   ORABLOCK_END
}

ORAEXT void oraMangoFlushInserts (OCIExtProcContext *ctx, int commit)
{
   ORABLOCK_BEGIN
   {
      OracleEnv env(ctx, logger);
      int i;

      for (i = MangoContext::begin(); i != MangoContext::end(); i = MangoContext::next(i))
      {
         env.dbgPrintfTS("flushing inserts of context #%d\n", i);

         MangoOracleContext &context = MangoOracleContext::get(env, i, false);

         context.fingerprints.flush(env);
         if (commit)
            OracleStatement::executeSingle(env, "COMMIT");
         context.context().unlock(env);
      }
   }
   ORABLOCK_END
}
