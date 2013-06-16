/*=========================================================================

  Program: DICOM for VTK

  Copyright (c) 2012-2013 David Gobbi
  All rights reserved.
  See Copyright.txt or http://www.cognitive-antics.net/bsd3.txt for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkDICOMMetaData.h"
#include "vtkDICOMParser.h"
#include "vtkDICOMReader.h"
#include "vtkDICOMSorter.h"
#include "vtkDICOMToRAS.h"
#include "vtkNIFTIWriter.h"

#include <vtkMatrix4x4.h>
#include <vtkStringArray.h>
#include <vtkIntArray.h>
#include <vtkErrorCode.h>
#include <vtkSmartPointer.h>

#include <vtksys/SystemTools.hxx>
#include <vtksys/Glob.hxx>

#include <string>
#include <vector>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// Simple structure for command-line options
struct dicomtonifti_options
{
  bool no_slice_reordering;
  bool no_row_reordering;
  bool no_column_reordering;
  bool no_qform;
  bool no_sform;
  bool batch;
  bool silent;
  bool verbose;
  const char *output;
};

// The help for the application
void dicomtonifti_usage(const char *command_name)
{
  const char *cp = command_name + strlen(command_name);
  while (cp != command_name && cp[-1] != '\\' && cp[-1] != '/') { --cp; }

  fprintf(stderr,
    "usage: %s -o file.nii file1.dcm [file2.dcm ...]\n", cp);
  fprintf(stderr,
    "       %s -o directory --batch file1.dcm [file2.dcm ...]\n", cp);
  fprintf(stderr,
    "options:\n"
    "  -o <output.nii[.gz]>    The output file.\n"
    "  --no-slice-reordering   Never reorder the slices.\n"
    "  --no-row-reordering     Never reorder the rows.\n"
    "  --no-column-reordering  Never reorder the columns.\n"
    "  --no-qform              Don't include a qform in the NIFTI file.\n"
    "  --no-sform              Don't include an sform in the NIFTI file.\n"
    "  --batch                 Do multiple series at once.\n"
    "  --silent                Do not echo output filenames.\n"
    "  --verbose               Verbose error reporting.\n"
  );
  fprintf(stderr,
    "\n");
  fprintf(stderr,
    "This program will convert a DICOM series into a NIfTI file.\n"
    "\n"
    "It reads the DICOM Position and Orientation metadata, and uses this\n"
    "information to generate qform and sform entries for the NIfTI header,\n"
    "after doing a conversion from the DICOM coordinate system to the NIfTI\n"
    "coordinate system.\n"
    "\n"
    "By default, it will also reorder the columns of the image so that\n"
    "columns with higher indices are further to the patient\'s right (or\n"
    "in the case of sagittal images, further anterior).  Likewise, rows\n"
    "will be rearranged so that rows with higher indices are superior (or\n"
    "anterior for axial images).  Finally, it will reorder the slices\n"
    "so that the column direction, row direction, and slice direction\n"
    "follow the right-hand rule.\n"
    "\n"
    "If batch mode is enabled, then the filenames will automatically be\n"
    "generated from the series description in the DICOM meta data:\n"
    "\"PatientName/StudyDescription-StudyID/SeriesDescription.nii.gz\".\n"
  );
}

// Print error
void dicomtonifti_check_error(vtkObject *o)
{
  vtkDICOMReader *reader = vtkDICOMReader::SafeDownCast(o);
  vtkDICOMSorter *sorter = vtkDICOMSorter::SafeDownCast(o);
  vtkNIFTIWriter *writer = vtkNIFTIWriter::SafeDownCast(o);
  vtkDICOMParser *parser = vtkDICOMParser::SafeDownCast(o);
  const char *filename = 0;
  unsigned long errorcode = 0;
  if (writer)
    {
    filename = writer->GetFileName();
    errorcode = writer->GetErrorCode();
    }
  else if (reader)
    {
    filename = reader->GetInternalFileName();
    errorcode = reader->GetErrorCode();
    }
  else if (sorter)
    {
    filename = sorter->GetInternalFileName();
    errorcode = sorter->GetErrorCode();
    }
  else if (parser)
    {
    filename = parser->GetFileName();
    errorcode = parser->GetErrorCode();
    }
  if (!filename)
    {
    filename = "";
    }

  switch(errorcode)
    {
    case vtkErrorCode::NoError:
      return;
    case vtkErrorCode::FileNotFoundError:
      fprintf(stderr, "File not found: %s\n", filename);
      break;
    case vtkErrorCode::CannotOpenFileError:
      fprintf(stderr, "Cannot open file: %s\n", filename);
      break;
    case vtkErrorCode::UnrecognizedFileTypeError:
      fprintf(stderr, "Unrecognized file type: %s\n", filename);
      break;
    case vtkErrorCode::PrematureEndOfFileError:
      fprintf(stderr, "File is truncated: %s\n", filename);
      break;
    case vtkErrorCode::FileFormatError:
      fprintf(stderr, "Bad DICOM file: %s\n", filename);
      break;
    case vtkErrorCode::NoFileNameError:
      fprintf(stderr, "Output filename could not be used: %s\n", filename);
      break;
    case vtkErrorCode::OutOfDiskSpaceError:
      fprintf(stderr, "Out of disk space while writing file: %s\n", filename);
      break;
    default:
      fprintf(stderr, "An unknown error occurred.\n");
      break;
    }

  exit(1);
}

// Add a dicom file to the list, expand if wildcard
void dicomtonifti_add_file(vtkStringArray *files, const char *filepath)
{
#ifdef _WIN32
  bool ispattern = false;
  bool hasbackslash = false;
  size_t n = strlen(filepath);
  for (size_t i = 0; i < n; i++)
    {
    if (filepath[i] == '*' || filepath[i] == '?' || filepath[i] == '[')
      {
      ispattern = true;
      }
    if (filepath[i] == '\\')
      {
      hasbackslash = true;
      }
    }

  std::string newpath = filepath;
  if (hasbackslash)
    {
    // backslashes interfere with vtksys::Glob
    vtksys::SystemTools::ConvertToUnixSlashes(newpath);
    }
  filepath = newpath.c_str();

  if (ispattern)
    {
    vtksys::Glob glob;
    if (glob.FindFiles(filepath))
      {
      const std::vector<std::string> &globfiles = glob.GetFiles();
      size_t m = globfiles.size();
      for (size_t j = 0; j < m; j++)
        {
        files->InsertNextValue(globfiles[j]);
        }
      }
    else
      {
      fprintf(stderr, "Could not match pattern: %s\n", filepath);
      exit(1);
      }
    }
  else
    {
    files->InsertNextValue(filepath);
    }
#else
  files->InsertNextValue(filepath);
#endif
}

// Read the options
void dicomtonifti_read_options(
  int argc, char *argv[],
  dicomtonifti_options *options, vtkStringArray *files)
{
  options->no_slice_reordering = false;
  options->no_row_reordering = false;
  options->no_column_reordering = false;
  options->no_qform = false;
  options->no_sform = false;
  options->batch = false;
  options->silent = false;
  options->verbose = false;
  options->output = 0;

  // read the options from the command line
  int argi = 1;
  while (argi < argc)
    {
    const char *arg = argv[argi++];
    if (arg[0] == '-')
      {
      if (strcmp(arg, "--") == 0)
        {
        // stop processing switches
        break;
        }
      else if (strcmp(arg, "--no-slice-reordering") == 0)
        {
        options->no_slice_reordering = true;
        }
      else if (strcmp(arg, "--no-row-reordering") == 0)
        {
        options->no_row_reordering = true;
        }
      else if (strcmp(arg, "--no-column-reordering") == 0)
        {
        options->no_column_reordering = true;
        }
      else if (strcmp(arg, "--no-qform") == 0)
        {
        options->no_qform = true;
        }
      else if (strcmp(arg, "--no-sform") == 0)
        {
        options->no_sform = true;
        }
      else if (strcmp(arg, "--batch") == 0)
        {
        options->batch = true;
        }
      else if (strcmp(arg, "--silent") == 0)
        {
        options->silent = true;
        }
      else if (strcmp(arg, "--verbose") == 0)
        {
        options->verbose = true;
        }
      else if (strncmp(arg, "-o", 2) == 0)
        {
        if (arg[2] != '\0')
          {
          arg += 2;
          }
        else
          {
          if (argi + 1 >= argc)
            {
            fprintf(stderr, "A file must follow the \'-o\' flag\n");
            dicomtonifti_usage(argv[0]);
            exit(1);
            }
          arg = argv[argi++];
          }
        options->output = arg;
        }
      }
    else
      {
      dicomtonifti_add_file(files, arg);
      }
    }

  while (argi < argc)
    {
    dicomtonifti_add_file(files, argv[argi++]);
    }
}

// Remove all characters but A-ZA-z0-9_ from a string
std::string dicomtonifti_safe_string(const std::string& str)
{
  std::string out;

  size_t n = str.size();
  size_t m = 0;
  for (size_t i = 0; i < n; i++)
    {
    char c = str[i];
    if (!isalnum(c))
      {
      c = '_';
      }
    else
      {
      m = i + 1;
      }
    out.push_back(c);
    }

  out.resize(m);

  if (out.size() == 0)
    {
    out = "UNKNOWN";
    }

  return out;
}

// Generate an output filename from meta data
std::string dicomtonifti_make_filename(
  const char *outpath, vtkDICOMMetaData *meta)
{
  std::string patientName = dicomtonifti_safe_string(
    meta->GetAttributeValue(DC::PatientName).AsString());
  std::string patientID = dicomtonifti_safe_string(
    meta->GetAttributeValue(DC::PatientID).AsString());
  std::string studyDesc = dicomtonifti_safe_string(
    meta->GetAttributeValue(DC::StudyDescription).AsString());
  std::string studyID = dicomtonifti_safe_string(
    meta->GetAttributeValue(DC::StudyID).AsString());
  std::string seriesDesc = dicomtonifti_safe_string(
    meta->GetAttributeValue(DC::SeriesDescription).AsString());

  if (patientName != "UNKNOWN")
    {
    patientID = patientName;
    }

  std::vector<std::string> sv;

  sv.push_back(outpath);
  sv.push_back(patientID);
  sv.push_back(studyDesc + "-" + studyID);
  sv.push_back(seriesDesc + ".nii.gz");

  return vtksys::SystemTools::JoinPath(sv);
}
 
// Convert one DICOM series into one NIFTI file
void dicomtonifti_convert_one(
  const dicomtonifti_options *options, vtkStringArray *a,
  const char *outfile)
{
  // read the files
  vtkSmartPointer<vtkDICOMReader> reader =
    vtkSmartPointer<vtkDICOMReader>::New();
  reader->SetMemoryRowOrderToFileNative();
  reader->SetFileNames(a);
  reader->Update();
  dicomtonifti_check_error(reader);

  // check if slices were reordered by the reader
  vtkIntArray *fileIndices = reader->GetFileIndexArray();
  vtkIdType maxId = fileIndices->GetMaxId();
  bool slicesReordered = (maxId > 0 &&
    fileIndices->GetValue(0) > fileIndices->GetValue(maxId));

  // convert to NIFTI coordinate system
  vtkSmartPointer<vtkDICOMToRAS> converter =
    vtkSmartPointer<vtkDICOMToRAS>::New();
  converter->SetInputConnection(reader->GetOutputPort());
  converter->SetPatientMatrix(reader->GetPatientMatrix());
  converter->SetAllowRowReordering(!options->no_row_reordering);
  converter->SetAllowColumnReordering(!options->no_column_reordering);
  converter->UpdateMatrix();

  // check if slices have been reordered by vtkDICOMToRAS
  vtkSmartPointer<vtkMatrix4x4> checkMatrix =
    vtkSmartPointer<vtkMatrix4x4>::New();
  checkMatrix->DeepCopy(reader->GetPatientMatrix());
  // undo the DICOM to NIFTI x = -x, y = -y conversion in check matrix
  for (int j = 0; j < 4; j++)
    {
    checkMatrix->Element[0][j] = -checkMatrix->Element[0][j];
    checkMatrix->Element[1][j] = -checkMatrix->Element[1][j];
    }
  checkMatrix->Invert();
  // checkMatrix = PatientMatrix^(-1) * RASMatrix
  vtkMatrix4x4::Multiply4x4(
    checkMatrix, converter->GetRASMatrix(), checkMatrix);
  // if z is negative, slices were reordered by vtkDIOCOMToRAS
  slicesReordered ^= (checkMatrix->GetElement(2, 2) < -0.1);

  // prepare the writer to write the image
  vtkSmartPointer<vtkNIFTIWriter> writer =
    vtkSmartPointer<vtkNIFTIWriter>::New();
  writer->SetFileName(outfile);
  if (options->no_slice_reordering && slicesReordered)
    {
    // force NIFTI file to store images in original DICOM order
    writer->SetQFac(-1.0);
    }
  if (!options->no_qform)
    {
    writer->SetQFormMatrix(converter->GetRASMatrix());
    }
  if (!options->no_sform)
    {
    writer->SetSFormMatrix(converter->GetRASMatrix());
    }
  writer->SetInputConnection(converter->GetOutputPort());
  writer->Write();
  dicomtonifti_check_error(writer);
}

// This program will convert DICOM to NIFTI
int main(int argc, char *argv[])
{
  // for the list of input DICOM files
  vtkSmartPointer<vtkStringArray> files =
    vtkSmartPointer<vtkStringArray>::New();

  dicomtonifti_options options;
  dicomtonifti_read_options(argc, argv, &options, files); 

  // whether to silence VTK warnings and errors
  vtkObject::SetGlobalWarningDisplay(options.verbose);

  // the output (NIFTI file or directory)
  const char *outpath = options.output;
  if (!outpath)
    {
    fprintf(stderr, "No output file was specified (\'-o\' <filename>).\n");
    dicomtonifti_usage(argv[0]);
    exit(1);
    }

  bool isDirectory = vtksys::SystemTools::FileIsDirectory(outpath);
  if (options.batch && !isDirectory)
    {
    fprintf(stderr, "In batch mode, -o must give an existing directory.\n");
    exit(1);
    }
  else if (!options.batch && isDirectory)
    {
    fprintf(stderr, "The -o option must give a file, not a directory.\n");
    exit(1);
    }

  // make sure that input files were provided
  if (files->GetNumberOfValues() == 0)
    {
    fprintf(stderr, "No input files were specified.\n");
    dicomtonifti_usage(argv[0]);
    exit(1);
    }

  // sort the files by study and series
  vtkSmartPointer<vtkDICOMSorter> sorter =
    vtkSmartPointer<vtkDICOMSorter>::New();
  sorter->SetInputFileNames(files);
  sorter->Update();
  dicomtonifti_check_error(sorter);

  if (!options.batch)
    {
    dicomtonifti_convert_one(
      &options, sorter->GetOutputFileNames(), outpath);
    }
  else
    {
    vtkSmartPointer<vtkDICOMParser> parser =
      vtkSmartPointer<vtkDICOMParser>::New();
    vtkSmartPointer<vtkDICOMMetaData> meta =
      vtkSmartPointer<vtkDICOMMetaData>::New();
    parser->SetMetaData(meta);

    int m = sorter->GetNumberOfStudies();
    for (int j = 0; j < m; j++)
      {
      int k = sorter->GetFirstSeriesInStudy(j);
      int n = sorter->GetNumberOfSeriesInStudy(j);
      n += k;
      for (; k < n; k++)
        {
        // get metadata of first file
        vtkStringArray *a = sorter->GetFileNamesForSeries(k);
        std::string fname = a->GetValue(0);
        meta->Clear();
        parser->SetFileName(fname.c_str());
        parser->Update();
        dicomtonifti_check_error(parser);

        // generate a filename from the meta data
        std::string outfile =
          dicomtonifti_make_filename(outpath, meta);

        // make the directory for the file
        if (k == sorter->GetFirstSeriesInStudy(j))
          {
          std::string dirname = vtksys::SystemTools::GetParentDirectory(
            outfile.c_str());
          if (!vtksys::SystemTools::MakeDirectory(dirname.c_str()))
            {
            fprintf(stderr, "Cannot create directory: %s\n",
                    dirname.c_str());
            exit(1);
            }
          }

        if (!options.silent)
          {
          printf("%s\n", outfile.c_str());
          }

        // convert the file
        dicomtonifti_convert_one(&options, a, outfile.c_str());
        }
      }
    }

  return 0;
}