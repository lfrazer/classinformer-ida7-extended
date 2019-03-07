
// ****************************************************************************
// File: Vftable.cpp
// Desc: Virtual function table parsing support
//
// ****************************************************************************
#include "stdafx.h"
#include "Core.h"
#include "Vftable.h"
#include "RTTI.h"

/*
namespace vftable
{
	int tryKnownMember(LPCTSTR name, ea_t eaMember);
};
*/

// Attempt to get information of and fix vftable at address
// Return TRUE along with info if valid vftable parsed at address
BOOL vftable::getTableInfo(ea_t ea, vtinfo &info)
{
    ZeroMemory(&info, sizeof(vtinfo));

	// Start of a vft should have an xref and a name (auto, or user, etc).
    // Ideal flags 32bit: FF_DWRD, FF_0OFF, FF_REF, FF_NAME, FF_DATA, FF_IVL
    //dumpFlags(ea);
    flags_t flags = get_flags(ea);
	if(has_xref(flags) && has_any_name(flags) && (isEa(flags) || is_unknown(flags)))
    {
        // Get raw (auto-generated mangled, or user named) vft name
        //if (!get_name(BADADDR, ea, info.name, SIZESTR(info.name)))
        //    msg(EAFORMAT" ** vftable::getTableInfo(): failed to get raw name!\n", ea);

        // Determine the vft's method count
        ea_t start = info.start = ea;
        while (TRUE)
        {
            // Should be an ea_t offset to a function here (could be unknown if dirty IDB)
            // Ideal flags for 32bit: FF_DWRD, FF_0OFF, FF_REF, FF_NAME, FF_DATA, FF_IVL
            //dumpFlags(ea);
            flags_t indexFlags = get_flags(ea);
            if (!(isEa(indexFlags) || is_unknown(indexFlags)))
            {
                //msg(" ******* 1\n");
                break;
            }

            // Look at what this (assumed vftable index) points too
            ea_t memberPtr = getEa(ea);
            if (!(memberPtr && (memberPtr != BADADDR)))
            {
                // vft's often have a zero ea_t (NULL pointer?) following, fix it
                if (memberPtr == 0)
                    fixEa(ea);

                //msg(" ******* 2\n");
                break;
            }

            // Should see code for a good vft method here, but it could be dirty
            flags_t flags = get_flags(memberPtr);
            if (!(is_code(flags) || is_unknown(flags)))
            {
                //msg(" ******* 3\n");
                break;
            }

            if (ea != start)
            {
                // If we see a ref after first index it's probably the beginning of the next vft or something else
                if (has_xref(indexFlags))
                {
                    //msg(" ******* 4\n");
                    break;
                }

                // If we see a COL here it must be the start of another vftable
                if (RTTI::_RTTICompleteObjectLocator::isValid(memberPtr))
                {
                    //msg(" ******* 5\n");
                    break;
                }
            }

            // As needed fix ea_t pointer, and, or, missing code and function def here
            fixEa(ea);
            fixFunction(memberPtr);

            ea += sizeof(ea_t);
        };

        // Reached the presumed end of it
        if ((info.methodCount = ((ea - start) / sizeof(ea_t))) > 0)
        {
            info.end = ea;
            //msg(" vftable: "EAFORMAT"-"EAFORMAT", methods: %d\n", rtInfo.eaStart, rtInfo.eaEnd, rtInfo.uMethods);
            return(TRUE);
        }
    }

    //dumpFlags(ea);
    return(FALSE);
}


// Get relative jump target address
// TODO: fix for x64, this is almost certainly 32bit specific (lfrazer)
static ea_t getRelJmpTarget(ea_t eaAddress)
{
	BYTE bt = get_byte(eaAddress);
	if(bt == 0xEB)
	{
		bt = get_byte(eaAddress + 1);
		if(bt & 0x80)
			return(eaAddress + 2 - ((~bt & 0xFF) + 1));
		else
			return(eaAddress + 2 + bt);
	}
	else
	if(bt == 0xE9)
	{
		UINT dw = get_32bit(eaAddress + 1);
		if(dw & 0x80000000)
			return(eaAddress + 5 - (~dw + 1));
		else
			return(eaAddress + 5 + dw);
	}
	else
		return(BADADDR);
}



#define SN_constructor 1
#define SN_destructor  2
#define SN_vdestructor 3
#define SN_scalardtr   4
#define SN_vectordtr   5


// Try to identify and place known class member types

int vftable::tryKnownMember(LPCTSTR name, ea_t eaMember)
{
	int iType = 0;

	#define IsPattern(Address, Pattern) (find_binary(Address, Address+(SIZESTR(Pattern)/2), Pattern, 16, (SEARCH_DOWN | SEARCH_NOBRK | SEARCH_NOSHOW)) == Address)

	if(eaMember && (eaMember != BADADDR))
	{
		// Skip if it already has a name
		flags_t flags = get_flags((ea_t) eaMember);
		
		// who cares if its named already, maybe we want to append multiple class prefixes?
		//if(!has_name(flags) || has_dummy_name(flags))
		{
			// Should be code
			if(is_code(flags))
			{
				ea_t eaAddress = eaMember;

				// E9 xx xx xx xx   jmp   xxxxxxx

				// look-ahead bytes for debugging
				const ssize_t numLookaheadBytes = 32;
				unsigned char lookaheadBytes[numLookaheadBytes];
				get_bytes(lookaheadBytes, numLookaheadBytes, eaAddress);
				BYTE Byte = lookaheadBytes[0];

				if((Byte == 0xE9) ||(Byte == 0xEB))  // is this correct for x64?
				{
					return(tryKnownMember(name, getRelJmpTarget(eaAddress)));
				}
				
				//else if(IsPattern(eaAddress, " "))
				else
				{

					qstring oldFuncName = get_name(eaAddress);

					if (strstr(oldFuncName.c_str(), name) == NULL) // add name prefix if it is not already in the name string
					{
						char newFuncName[MAXSTR];

						//msg(EAFORMAT " Should set name of function? (Class=%s)\n", eaMember, name);

						_snprintf(newFuncName, MAXSTR, "%s_vf_%s", name, oldFuncName.c_str());

						set_name(eaAddress, newFuncName, (SN_NON_AUTO | SN_NOWARN | SN_NOCHECK));
					}
				}
			}
			else
				msg(" " EAFORMAT " ** Not code at this member! **\n", eaMember);
		}
	}

	return(iType);
}


/*
TODO: On hold for now.
Do we really care about detected ctors and dtors?
Is it helpful vs the problems of naming member functions?
*/


// Process vftable member functions

// TODO: Just try the fix missing function code
void vftable::processMembers(LPCTSTR lpszName, ea_t eaStart, ea_t eaEnd)
{
	//msg(" "EAFORMAT" to "EAFORMAT"\n", eaStart, eaEnd);

	ea_t eaAddress = eaStart;

	while(eaAddress < eaEnd)
	{
		ea_t eaMember;
		if(getVerifyEA_t(eaAddress, eaMember))
		{
			// Missing/bad code?
			if(!get_func(eaMember))
			{
				msg(" " EAFORMAT " ** No member function here! **\n", eaMember);
                fixFunction(eaMember);
			}

			tryKnownMember(lpszName, eaMember);
		}
		else
			msg(" " EAFORMAT " ** Failed to read member pointer! **\n", eaAddress);

		// lfrazer: increment by 4 probably for 32bit.. in x64 we have 8 byte ptrs
#ifdef __EA64__
		eaAddress += 8;
#else
		eaAddress += 4;
#endif
	};

	// lfrazer: Find constructors?
	// is vtbl start address at eaStart or eaStart - 1?
	qstring vtblName = get_name(eaStart);
	//msg("Getting xrefs for " EAFORMAT " (%s) to find CTORS\n", eaStart, vtblName.c_str());

	xrefblk_t vtblXrefs;
	bool success = xrefblk_t_first_to(&vtblXrefs, eaStart, XREF_FAR);

	while (success)
	{
		//msg(EAFORMAT " address of vtbl xref (code inside constructor?)\n", vtblXrefs.from);  // if we ask for "to" xrefs, the useful info is in "from" field
		
		flags_t currflags = get_flags(vtblXrefs.from);
		if (is_code(currflags))
		{
			func_t* ctorFunc = get_func(vtblXrefs.from);
			
			if (ctorFunc)
			{
				qstring fname = get_name(ctorFunc->start_ea);
				//msg(EAFORMAT " address of suspected CTOR %s()\n", ctorFunc->start_ea, fname.c_str());

				flags_t funcflags = get_flags(ctorFunc->start_ea);

				// rename the CTOR if it doesn't have a name yet
				if (!has_name(funcflags) || has_dummy_name(funcflags))
				{
					char newFuncName[MAXSTR];
					_snprintf(newFuncName, MAXSTR, "%s_CTOR", lpszName);

					set_name(ctorFunc->start_ea, newFuncName, (SN_NON_AUTO | SN_NOWARN | SN_NOCHECK));
					
					// add comment about original func name
					char commentOrigName[MAXSTR];
					_snprintf(commentOrigName, MAXSTR, "orig funcname: %s", fname.c_str());
					set_cmt(ctorFunc->start_ea, commentOrigName, false);
				}
				else
				{
					//msg(EAFORMAT " address of suspected CTOR for %s but already has a name: %s\n", ctorFunc->start_ea, lpszName, fname.c_str());

					// In this case we should still rename it (IF it was named by us), since many constructors will reference vtbls of sub classes too, but they are processed later on in data section

					if (strstr(fname.c_str(), "_CTOR") != NULL)
					{
						char newFuncName[MAXSTR];
						_snprintf(newFuncName, MAXSTR, "%s_CTOR", lpszName);

						set_name(ctorFunc->start_ea, newFuncName, (SN_NON_AUTO | SN_NOWARN | SN_NOCHECK));
					}
				}
				
			}
		}

		success = xrefblk_t_next_to(&vtblXrefs);
	}

}
