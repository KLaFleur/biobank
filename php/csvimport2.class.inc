<?php declare(strict_types=1);
namespace LORIS\biobank;

use \Psr\Http\Message\ServerRequestInterface;
use \Psr\Http\Message\ResponseInterface;
use \LORIS\biobank\DataRowAddEdit;
use LORIS\StudyEntities\Candidate\CandID;
use \LORIS\StudyEntities\Candidate\Sex;

/**
 * Post method handles add.
 * This class contains functions providing add and edit functionality.
 *
 * @license http://www.gnu.org/licenses/gpl-3.0.txt GPLv3
 */
class CSVImport extends \LORIS\cbigr_api\Endpoint
{
    public $skipTemplate = true;

    /**
     * Same as the permissions for the main menu filter page.
     *
     * @param \User $user The user whose access is being checked
     *
     * @return bool true only if the user has access to this page.
     */
    function _hasAccess(\User $user) : bool
    {
        return $user->hasAnyPermission(
            [
                'biobank_specimen_create'
            ]
        );
    }


    protected function allowedMethods(): array
    {
        return [
            "GET",
            "POST",
        ];
    }

    /**
     * Handle how to operate all the files.
     * POST method uploads a file.
     * GET method gets a file.
     *
     * @param ServerRequestInterface $request The incoming PSR7 request
     *
     * @return ResponseInterface The outgoing PSR7 response
     */
    public function handle(ServerRequestInterface $request): ResponseInterface
    {
        switch ($request->getMethod()) {
            case "GET":
                return $this->downloadErrorLog($request);
            case "POST":
                return $this->importCSV($request);
            default:
                return (new \LORIS\Http\Response\JSON\MethodNotAllowed(
                    $this->allowedMethods()
                ));
        }
    }

    /**
     * Handles the document editing process
     *
     * @param ServerRequestInterface $request The incoming PSR7 request
     *
     * @return ResponseInterface
     */
    function importCSV(ServerRequestInterface $request): ResponseInterface
    {
        $loris = $request->getAttribute('loris');
        $DB = $loris->getDatabaseConnection();
        $fileDate = date('Ymd_His');

        $targetdir = "/data/biobank/imports/";
        $logdir = "/data/biobank/logs/";
        // Ensure the server is properly configured.
        if (!is_writable($targetdir)) {
            return new \LORIS\Http\Response\JSON\Forbidden(
                "User does not have write permission on filesystem"
            );
        }

        $uploaded_file = $request->getUploadedFiles()['csvFile'];
        $spl           = new \SplFileInfo($targetdir);
        $filename      = $uploaded_file->getClientFilename(); //blabla.csv
        $fullPath      = $spl->getPathname().'/'.$filename; // /path/to/imports/blabla.csv
        $fullPathDatedNoExt = $spl->getPathname().'/'.basename($filename,".csv")."_".$fileDate; // /path/to/imports/blabla_20231001_123456
        $fullPathDated = $fullPathDatedNoExt.".csv"; // /path/to/imports/blabla_20231001_123456.csv
        $logPathDated = str_replace("/imports/","/logs/",$fullPathDated); // /path/to/logs/blabla_20231001_123456.csv

        $uploader = (new \LORIS\FilesUploadHandler($spl))
            ->withPermittedMIMETypes(
                'text/csv',
                'application/vnd.ms-excel',
            );

        $response = $uploader->handle($request);

        if (!in_array($response->getStatusCode(), [200, 201], true)) {
            return $response;
        }
        // move file after upload because FileUploadHandler
        // can't rename file to append a date
        rename($fullPath,$fullPathDated);

        $output = $this->parseCSV($fullPathDated, $logPathDated, $DB);

        return new \LORIS\Http\Response\JSON\Created(
            [
                'message' => count($output['new'])." New Entries. ".
                    count($output['errors'])." Errors. ".
                    count($output['ignored'])." Ignored.",
                'log' => basename($logPathDated),
            ]
        );
    }

    /**
     * function dedicated to the parse logic
     * 
     * This function can be run using the tools/CSVImportRunner.php
     * 
     * @param string    $fullPathDated Path to the CSV that needs to be parsed
     * @param string    $logPathDated  Path to the log files
     * @param \Database $DB            Database
     * 
     * @return Array $arr Array containing catalogue information 
     * 
     */
    public function parseCSV(string $fullPathDated, string $logPathDated, \Database $DB) : array
    {
        // open file and start parsing CSV
        $file = fopen($fullPathDated,"r");
        $headers = fgetcsv($file);
        $logActive = fopen($logPathDated.".active.log","w");


        //Placeholders
        $errors = [];
        $missingIIDCount = 0;
        $missingIIDMapping =[];
        $ignored = [];
        $new = [];
        $failed = false;

        //Entities
        $NBCextID = $DB->pselectOne(
            "SELECT ProjectExternalID FROM Project_external WHERE Name=:name",
            ["name"=>"NBC"]
        );
        $NBCProjectID = \Project::singleton("Neuro Bioinformatics Core")->getId();
        $MNICenterID = new \CenterID($DB->pselectOne("SELECT CenterID FROM psc WHERE Name='Montreal Neurological Institute'",[]));
        $defaultDOB = "1000-01-01";

        //
        $uniqueList = $DB->pselectCol(
            "SELECT CONCAT(ExtStudyID, '_',SAMPLE_ID,'_',SEQUENCING_METHOD, '_' ,COALESCE(BATCH,'NA'), '_' ,COALESCE(LIBRARY_TYPE,'NA')) 
            FROM genomics_catalogue gs 
                JOIN candidate_project_extid_rel cper ON gs.CandID=cper.CandID 
                JOIN Project_external pe ON pe.ProjectExternalID=cper.ProjectExternalID 
            WHERE pe.Name=:name",
            ["name"=>"NBC"]
        );

        // Hack for duplicate candidates
        $extIDList = $DB->pselectColWithIndexKey(
            "SELECT ExtStudyID, MAX(cper.CandID) 
                FROM candidate_project_extid_rel cper
                JOIN Project_external pe ON pe.ProjectExternalID=cper.ProjectExternalID 
                WHERE pe.Name NOT IN ('TOSI_mismatch','DEMS_mismatch')
                GROUP BY cper.ExtStudyID",
            [],
            "ExtStudyID"
        );

        $PSCIDList = $DB->pselectColWithIndexKey(
            "SELECT CandID, PSCID FROM candidate WHERE PSCID <> 'scanner'",
            [],
            "PSCID"
        );

        $specimen = $DB->pselectWithIndexKey(
            "SELECT Barcode, SpecimenID, c.CandID
                FROM biobank_container bc 
                    JOIN biobank_specimen bs on bs.ContainerID = bc.ContainerID 
                    JOIN biobank_specimen_type bst ON bs.SpecimenTypeID = bst.SpecimenTypeID
                    JOIN biobank_container_status bcs ON bcs.ContainerStatusID = bc.ContainerStatusID
                    JOIN session s ON s.ID = bs.SessionID
                    JOIN candidate c ON c.CandID = s.CandID
                WHERE bst.Label IN ('DNA','RNA') AND bcs.Label <> 'Available'",
            [],
            "Barcode"
        );

        $libraryTypeOptions = $DB->pselectCol(
            "SELECT Name FROM genomics_catalogue_library_types",
            []
        );

        //Get CSV data to import
        while (($line=fgetcsv($file)) !== FALSE) {
            // Override default exectuion time to 10 seconds per iteration 
            // (still cathces infinite loops)
            set_time_limit(10);
            
            $failed = false;
            // Combining headers with values and running through array_filter
            // to remove empty strings and NA values
            $row = array_filter(
                array_combine($headers,$line),
                function($cell) {
                    return !empty($cell) && strtolower($cell) !== "na";
                }
            );

            /**
             * 1. Check if there is an INDIVIDUAL ID in the row => if not COUNT & SKIP
             * 2. Check if data is imported for this INDIVIDUAL + SAMPLE + SEQUENCE + BATCH => if so IGNORE & SKIP
             * 3. Prepare data for validation
             * 4. Validate minimum required fields + enum fields => if problem ERROR
             * 5. Check if INDIVIDUAL ID is already in our DB => if so #7 | if not #6
             * 6. Create Candidate (if need be)
             * 7. Check if one of the sample aliases maps to an existing biospecimen barcode
             * 8. Prepare data for insert
             * 9. Insert data into respective tables
             * 10. Add family relation to insert list
             */

            // If there is no NBC individualID assume uniqueness comes from SampleID
            if (!isset($row['INDIVIDUAL_ID'])) {
                $missingIIDCount++;
                // Set empty array key until new candidate fills the value
                $missingIIDMapping[$row['SAMPLE_ID']] = null;

                //Temporarily set the Individual ID to placeholder until its filled
                $row['INDIVIDUAL_ID'] = "TEMP";
            }

            //Null coalesce variables in case they are missing (for error reporting)
            $ukey = $row['INDIVIDUAL_ID']."_".($row['SAMPLE_ID'] ?? "NA")."_".($row['SEQUENCING_METHOD'] ?? "NA")."_".($row['BATCH'] ?? "NA")."_".($row['LIBRARY_TYPE'] ?? "NA");
            fwrite(
                $logActive,
                $ukey."\n"
            );

            // Check if another cell line has the same unique key in this same file
            if (in_array($ukey,array_merge($ignored,$new),true) || array_key_exists($ukey,$errors)) {
                //Checked the NEW and IGNORED values and this is a duplicate
                $errors[$ukey]['duplication'] = "DUPLICATE IN SAME FILE";
                $failed = true;
                fwrite(
                    $logActive,
                    $ukey." --> duplication --> DUPLICATE IN SAME FILE\n"
                );
            }
            
            // Check if its already in the database from a previous import
            if (in_array($ukey, $uniqueList, true)) {
                $ignored[] = $ukey;
                $failed = true;
            }

            /**
             * VALIDATE
             */
            $dataErrors = DataRowAddEdit::validate($row, $DB); 
            if (!empty($dataErrors)) {
                $errors[$ukey]['data'] = $dataErrors;
                $failed = true;
                fwrite(
                    $logActive,
                    $ukey." --> data --> ".print_r($dataErrors,true)."\n"
                );
            }

            //Check if row failed for any reason, skip
            if ($failed) {continue;}
            
            /**
             * Data is valid, proceed to candidate management
             */
            $cid = null;
            $specimenID = null;
            $specimenCID = null;
            $aliases = [];

            // Check first if candidate already exists
            $cid = $extIDList[$row['INDIVIDUAL_ID']] ?? null;

            // Extract the SAMPLE_ALIAS data, we have to do this to match 
            // to a specimen barcode whether candidate exists or not
            if (isset($row['SAMPLE_ALIAS'])) {
                $aliases = explode('/',$row['SAMPLE_ALIAS']);
                foreach ($aliases as $alias) {
                    if (array_key_exists($alias,$specimen)) {
                        $specimenID = $specimen[$alias]['SpecimenID'];
                        // Since we are getting the info, we may as well get the cid
                        $specimenCID = $specimen[$alias]['CandID'];
                        break;
                    }
                }
            }
            // If still no cid, check if the sample alias is a PSCID or ExtStudyID
            if (empty($cid)) {
                /** 
                 * Candidate is not already registered
                 * 
                 * Possibilities:
                 * 1. Candidate exists but not linked to individual ID
                 *     a. Sample Alias matched a Barcode
                 *     b. Sample Alias matched a PSCID
                 *     c. Sample Alias matched an external ID
                 * 2. candidate does not exist
                 *      - Create a new one
                 *      - add external identifier for NBC
                 * 
                 * No need to check specimen since its not a CBIG candidate
                */

                // 1. Candidate exists but is not linked to individual ID 
                // a. check if it matched a barcode earlier and set it
                // b/c. parse the Sample alias on all its parts, 
                // check either one for a match on PSCID or extID
                if (isset($specimenCID)) {
                    // Not setting it earlier to leverage insertion into extIDList
                    $cid = $specimenCID; 

               } else {
                    foreach ($aliases as $alias) {
                        if (array_key_exists($alias,$extIDList)) {
                            $cid = $extIDList[$alias];
                        } elseif (array_key_exists($alias,$PSCIDList)) {
                            $cid = $PSCIDList[$alias];
                        }
                    }
                }
                
                // if STILL empty cid, create one
                if (empty($cid)) {
                    // 2. candidate does not exist
                    $cid = (string) \Candidate::createNew(
                        $MNICenterID,
                        $defaultDOB,
                        null,
                        new Sex($row['SEX'] ?? 'Unknown'),
                        null,
                        $NBCProjectID,
                    );
                    fwrite(
                        $logActive,
                        $ukey." --> New Candidate --> ".print_r($cid,true)."\n"
                    );
                }

                // A match was found or a patient created, either way make sure 
                // INDIVIDUAL ID external ID is recorded and move on
                try {
                    // Right before inserting, check if Individual ID was just a 
                    // place holder and repalce it with PSCID (either new or existent)
                    if ($row['INDIVIDUAL_ID'] === "TEMP") {
                        $row['INDIVIDUAL_ID'] = \Candidate::singleton(new CandID($cid))->getPSCID();
                        $missingIIDMapping[$row['SAMPLE_ID']] = $row['INDIVIDUAL_ID'];
                        fwrite(
                            $logActive,
                            $ukey." --> New Individual ID --> ".print_r($row['INDIVIDUAL_ID'],true)."\n"
                        );
                        //Regenerate the unique key with the new individual ID
                        $ukey = $row['INDIVIDUAL_ID']."_".($row['SAMPLE_ID'] ?? "NA")."_".($row['SEQUENCING_METHOD'] ?? "NA")."_".($row['BATCH'] ?? "NA")."_".($row['LIBRARY_TYPE'] ?? "NA");
                    }

                    $DB->unsafeinsert(
                        "candidate_project_extid_rel", 
                        [
                            "CandID"=>$cid,
                            "ProjectExternalID" => $NBCextID,
                            "ExtStudyID" => $row['INDIVIDUAL_ID'],
                        ]
                    );
                } catch (\DatabaseException | \PDOException $e) {
                    $errors[$ukey]['EXTID'] = $e->getMessage();
                    $failed = true;
                    fwrite(
                        $logActive,
                        $ukey." --> EXTID --> ".$e->getMessage()."\n"
                    );
                    continue;
                }

                $extIDList[$row['INDIVIDUAL_ID']] = (string) $cid;
            }

            /**
             * Insert data in respective tables
             * 
             * TODO: switch to insertmany function
             */
            if (!empty($specimenID)) {
                $row['SpecimenID'] = $specimenID; 

            }

            $row['CandID'] = (string) $cid;

            $rawNBC = isset($row['RAW_BELUGA_LOCATION']) ? array_unique(array_map('trim', explode(';', $row['RAW_BELUGA_LOCATION']))) : [];
            $analyzedNBC = isset($row['ANALYZED_BELUGA_LOCATION']) ? array_unique(array_map('trim', explode(';', $row['ANALYZED_BELUGA_LOCATION']))) : [];
            $rawCloud = isset($row['RAW_CLOUD_LOCATION']) ? array_unique(array_map('trim', explode(';', $row['RAW_CLOUD_LOCATION']))) : [];
            $analyzedCloud = isset($row['ANALYZED_CLOUD_LOCATION']) ? array_unique(array_map('trim', explode(';', $row['ANALYZED_CLOUD_LOCATION']))) : [];
            $diagnoses = isset($row['DISEASE']) ? array_unique(array_map('trim', explode(';', $row['DISEASE']))) : [];

            unset($row["INDIVIDUAL_ID"]);
            unset($row["RAW_BELUGA_LOCATION"]);
            unset($row["ANALYZED_BELUGA_LOCATION"]);
            unset($row["RAW_CLOUD_LOCATION"]);
            unset($row["ANALYZED_CLOUD_LOCATION"]);
            unset($row["DISEASE"]);

            //TODO use insertmany
            $DB->beginTransaction();
            try {
                // Before inserting data, add necessary options to library types
                if (isset($row['LIBRARY_TYPE']) && !in_array($row['LIBRARY_TYPE'], $libraryTypeOptions, true)) {
                    $DB->insert("genomics_catalogue_library_types", ['Name'=>$row['LIBRARY_TYPE']]);
                    $libraryTypeOptions[] = $row['LIBRARY_TYPE'];
                }

                $DB->unsafeinsert('genomics_catalogue', $row);
                $gsid = $DB->getLastInsertId();

                foreach ($rawNBC as $sfile) {
                    $DB->unsafeinsert('genomics_catalogue_files',['GenomicsCatalogueID'=>$gsid,'Location'=>'Beluga','Type'=>'Raw','Path'=>$sfile]);
                }
                foreach ($analyzedNBC as $sfile) {
                    $DB->unsafeinsert('genomics_catalogue_files',['GenomicsCatalogueID'=>$gsid,'Location'=>'Beluga','Type'=>'Analyzed','Path'=>$sfile]);
                }
                foreach ($rawCloud as $sfile) {
                    $DB->unsafeinsert('genomics_catalogue_files',['GenomicsCatalogueID'=>$gsid,'Location'=>'Cloud','Type'=>'Raw','Path'=>$sfile]);
                }
                foreach ($analyzedCloud as $sfile) {
                    $DB->unsafeinsert('genomics_catalogue_files',['GenomicsCatalogueID'=>$gsid,'Location'=>'Cloud','Type'=>'Analyzed','Path'=>$sfile]);
                }


                foreach ($diagnoses as $diagnosis) {
                    $DB->unsafeinsert('genomics_catalogue_diagnosis',['GenomicsCatalogueID'=>$gsid,'Diagnosis'=>$diagnosis]);
                }

                // Add to new arrsay only after DB insertion successful
                $new[] = $ukey;

            } catch (\DatabaseException | \PDOException $e) {
                $DB->rollBack();
                $errors[$ukey]['SQL'] = $e->getMessage();
                $failed = true;
                fwrite(
                    $logActive,
                    $ukey." --> SQL --> ".$e->getMessage()."\n"
                );

                // When rolling back refresh libraryTypeoptions in case an insert got rolled back
                $libraryTypeOptions = $DB->pselectCol(
                    "SELECT Name FROM genomics_catalogue_library_types",
                    []
                );
                continue;
            }
            $DB->commit();
            //TODO add family logic
        }

        fclose($file);

        $log = fopen($logPathDated.".log","w");
        $logPretty = fopen($logPathDated.".pretty.log","w");
        $arr = [
                "generated_individual_ids" => $missingIIDMapping,
                "errors" => $errors,
                "ignored" => $ignored,
                "missing_individual_ids" => $missingIIDCount,
                "new" => $new,
            ];
        fwrite(
            $log,
            json_encode($arr)
        );
        fwrite(
            $logPretty,
            print_r($arr,true)
        );
        fclose($log);
        fclose($logPretty);
        fclose($logActive);

        return $arr;
    }

    /**
     * Serve log associated to CSV import attempt
     *
     * @param ServerRequestInterface $request
     * @return ResponseInterface
     */
    function downloadErrorLog(ServerRequestInterface $request): ResponseInterface
    {
        $filename = urldecode(basename($request->getUri()->getPath()));

        $targetdir = "/data/biobank/logs/";
        $spl       = new \SplFileInfo($targetdir);

        $downloader = new \LORIS\FilesDownloadHandler($spl);

        return $downloader->handle(
            $request->withAttribute('filename', $filename.".pretty.log")
        );
    }


    /**
     * This function can be overridden in a module's page to load the necessary
     * resources to check the permissions of a user.
     *
     * @param \User                   $user    The user to load the resources for
     * @param ServerRequestInterface $request The PSR15 Request being handled
     *
     * @return void
     */
    public function loadResources(
        \User $user, ServerRequestInterface $request
    ) : void {
    }

}
